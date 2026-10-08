// 行为断言在所有构建配置中启用。
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "thread_pool.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

namespace {
// 本机 UCRT64 测试：先记录构造期间主线程的分配序列，再据此定位 worker 创建失败点。
// 每个 worker 的创建在主线程分配固定次数，其首次分配就是 work_thread 对象自身；
// 与依赖对象大小的写法相比，这里由测量结果推导顺序，假设不成立时断言会直接失败。
constexpr std::size_t recorded_capacity = 64;
thread_local bool record_allocations = false;
thread_local std::size_t recorded_sizes[recorded_capacity] = {};
thread_local std::size_t recorded_count = 0;
// 注入：第 worker_allocation_ordinal 次主线程分配抛 bad_alloc（从 1 起算）。
thread_local bool fail_worker_allocation = false;
thread_local std::size_t worker_allocation_ordinal = 0;
thread_local std::size_t worker_allocation_count = 0;
// 首个 worker 对象的分配序号及其地址，用于核对部分创建失败后已创建线程被回收。
thread_local std::size_t first_worker_object_ordinal = 0;
thread_local void* first_worker_object = nullptr;
thread_local bool first_worker_released = false;
// shutdown_now 的本地丢弃队列分配失败注入，测试线程局部，不影响 worker。
thread_local bool fail_shutdown_allocation = false;
// enqueue 入队分配失败注入：先放行固定次数的分配，再让这一次分配抛 bad_alloc。
thread_local bool fail_enqueue_allocation = false;
thread_local int enqueue_allocations_before_failure = 0;
// 每个登记过的 worker 线程把一个 thread_local 退出记录的地址登记进来。
std::mutex exit_records_mutex;
std::vector<std::unique_ptr<std::atomic<int>>> exit_records;
}

// 避免 Release 内联替换分配器后误报 mismatched-new-delete，保持注入调用可观察。
[[gnu::noinline]] void* operator new(std::size_t size) {
    if (record_allocations && recorded_count < recorded_capacity) {
        recorded_sizes[recorded_count++] = size;
    }
    if (fail_shutdown_allocation) {
        fail_shutdown_allocation = false;
        throw std::bad_alloc{};
    }
    if (fail_enqueue_allocation) {
        if (enqueue_allocations_before_failure == 0) {
            fail_enqueue_allocation = false;
            throw std::bad_alloc{};
        }
        --enqueue_allocations_before_failure;
    }
    if (fail_worker_allocation) {
        ++worker_allocation_count;
        if (worker_allocation_count == worker_allocation_ordinal) {
            fail_worker_allocation = false;
            throw std::bad_alloc{};
        }
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        if (fail_worker_allocation && worker_allocation_count == first_worker_object_ordinal) {
            first_worker_object = memory;
        }
        return memory;
    }
    throw std::bad_alloc{};
}
[[gnu::noinline]] void* operator new[](std::size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete(void* memory) noexcept {
    if (memory != nullptr && memory == first_worker_object) {
        first_worker_released = true;
    }
    std::free(memory);
}
[[gnu::noinline]] void operator delete[](void* memory) noexcept { ::operator delete(memory); }
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
[[gnu::noinline]] void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

struct thread_pool_test_access {
    static std::size_t worker_count(const thread_pool& pool) {
        return pool.worker_threads_.size();
    }
    static status_t state(thread_pool& pool) {
        std::lock_guard lock(pool.queue_mutex_);
        return pool.state_;
    }
    static std::size_t queued_count(thread_pool& pool) {
        std::lock_guard lock(pool.queue_mutex_);
        return pool.tasks_.size();
    }
    static void wait_for_closing(thread_pool& pool, status_t expected) {
        std::unique_lock lock(pool.queue_mutex_);
        assert(pool.task_ready_.wait_for(lock, 5s, [&] {
            return pool.state_ != status_t::RUNNING;
        }));
        assert(pool.state_ == expected);
    }
    static bool owner_cleared(thread_pool& pool) {
        std::lock_guard lock(pool.queue_mutex_);
        return pool.closing_thread_id_ == std::thread::id{};
    }
    static void stop(thread_pool& pool, status_t requested) { pool.stop(requested); }
};

namespace {
// 各线程的退出计数：线程退出时由 TLS 析构累加，登记线程清单由它维护。
// worker 退出记录：登记本线程的计数槽，线程退出时该槽加一。
struct worker_exit_record {
    std::atomic<int>* slot = nullptr;
    worker_exit_record() {
        auto owned = std::make_unique<std::atomic<int>>(0);
        slot = owned.get();
        std::lock_guard lock(exit_records_mutex);
        exit_records.push_back(std::move(owned));
    }
    ~worker_exit_record() { slot->fetch_add(1, std::memory_order_relaxed); }
};

// 任务体调用：在本线程构造退出记录，避免块作用域 thread_local 被优化掉。
struct worker_exit_touch {
    worker_exit_touch() {
        thread_local worker_exit_record record;
        (void)record;
    }
};

// 全部已登记 worker 线程的退出计数之和。
int registered_worker_exits() {
    std::lock_guard lock(exit_records_mutex);
    int total = 0;
    for (const auto& slot : exit_records) {
        total += slot->load(std::memory_order_relaxed);
    }
    return total;
}

// 让全新池的每个 worker 各执行一个登记任务，用于核对全部 worker 都已退出。
// 每个任务登记后等待共同放行信号：任务已在锁内被唯一 worker 领取，各 worker 各领一个。
void all_workers_run(thread_pool& pool, std::size_t worker_count) {
    std::latch claimed(static_cast<std::ptrdiff_t>(worker_count));
    std::latch release(1);
    std::vector<std::future<int>> results;
    results.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        results.push_back(pool.submit([&] {
            worker_exit_touch touch;
            claimed.count_down();
            release.wait();
            return 1;
        }));
    }
    claimed.wait(); // 全部 worker 已领取各自任务并完成登记。
    release.count_down();
    for (auto& result : results) {
        assert(result.get() == 1);
    }
}

// 捕获 unique_ptr 保持探针地址不变，准备阶段移动 callable 不会提前销毁它。
// 析构保持进行时，另一线程必须能取得队列锁；避免同线程重锁与 try_lock 伪失败。
struct lock_probe {
    thread_pool& pool;
    std::atomic<int>& destroyed;
    std::promise<std::thread::id>& signal;

    ~lock_probe() {
        auto observer = std::async(std::launch::async, [&] {
            return thread_pool_test_access::queued_count(pool);
        });
        assert(observer.wait_for(5s) == std::future_status::ready);
        (void)observer.get();
        destroyed.fetch_add(1, std::memory_order_relaxed);
        signal.set_value(std::this_thread::get_id());
    }
};

// 关闭完成核对：状态为 TERMINATED，负责人和 worker 集合已清空，重复关闭不增加退出计数。
// expected_worker_exits 为负表示本用例未登记全部 worker，不核对退出计数。
void check_terminated(thread_pool& pool, int expected_worker_exits) {
    assert(thread_pool_test_access::state(pool) == status_t::TERMINATED);
    assert(thread_pool_test_access::owner_cleared(pool));
    assert(thread_pool_test_access::worker_count(pool) == 0);
    const int exits = registered_worker_exits();
    pool.shutdown();
    pool.shutdown_now(); // 完成后重复关闭不再回收，也不准备丢弃队列。
    assert(registered_worker_exits() == exits); // 不重复回收，退出计数不再增加。
    if (expected_worker_exits >= 0) {
        assert(exits == expected_worker_exits);
    }
}

void check_broken_promise(std::future<int>& result) {
    bool broken = false;
    try { (void)result.get(); }
    catch (const std::future_error& error) {
        broken = error.code() == std::make_error_code(std::future_errc::broken_promise);
    }
    assert(broken);
}

// 指定数量与默认数量：显式参数按值创建，默认参数沿用 default_worker_count() 规则。
void requested_and_default_worker_count() {
    int exits = registered_worker_exits();
    {
        thread_pool pool(3);
        assert(thread_pool_test_access::worker_count(pool) == 3);
        all_workers_run(pool, 3); // 指定的 3 个 worker 都可执行任务。
        pool.shutdown();
        check_terminated(pool, exits + 3);
        exits += 3;
    }
    {
        // 默认数量：hardware_concurrency() 返回 0 时保底 1，与产品规则一致。
        const auto hardware_count = std::thread::hardware_concurrency();
        const std::size_t expected = hardware_count == 0 ? 1 : hardware_count;
        thread_pool pool;
        assert(thread_pool_test_access::worker_count(pool) == expected);
        assert(pool.submit([] {
                   worker_exit_touch touch;
                   return 17;
               }).get() == 17);
        pool.shutdown();
        check_terminated(pool, exits + 1); // 只有领取该任务的 worker 登记了退出记录。
    }
}

// 显式 worker_count=0 抛 invalid_argument。
void zero_worker_count_is_rejected() {
    const int exits_before = registered_worker_exits();
    bool rejected = false;
    try { thread_pool pool(0); }
    catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()).find("positive") != std::string_view::npos;
    }
    assert(rejected);
    assert(registered_worker_exits() == exits_before);

    // 拒绝后仍可用正常数量构造、执行任务、关闭并析构。
    thread_pool retry(2);
    assert(thread_pool_test_access::worker_count(retry) == 2);
    all_workers_run(retry, 2);
    retry.shutdown();
    check_terminated(retry, exits_before + 2);
}

// 创建部分 worker 失败时回收已创建线程。
void partial_construction_failure_reclaims_workers() {
    // 预热一次，排除本进程首次构造线程时可能出现的额外一次性分配。
    {
        thread_pool warmup(1);
        warmup.shutdown();
    }

    // 记录 1 个和 2 个 worker 的构造分配序列，由差值得到单个 worker 的分配组。
    std::array<std::size_t, recorded_capacity> one_worker_sizes{};
    std::size_t one_worker_count = 0;
    recorded_count = 0;
    record_allocations = true;
    {
        thread_pool probe(1);
        record_allocations = false;
        one_worker_count = recorded_count;
        for (std::size_t index = 0; index < one_worker_count; ++index) {
            one_worker_sizes[index] = recorded_sizes[index];
        }
        probe.shutdown();
    }
    std::array<std::size_t, recorded_capacity> two_worker_sizes{};
    std::size_t two_worker_count = 0;
    recorded_count = 0;
    record_allocations = true;
    {
        thread_pool probe(2);
        record_allocations = false;
        two_worker_count = recorded_count;
        for (std::size_t index = 0; index < two_worker_count; ++index) {
            two_worker_sizes[index] = recorded_sizes[index];
        }
        probe.shutdown();
    }
    assert(one_worker_count > 0 && two_worker_count > one_worker_count);
    const std::size_t group_length = two_worker_count - one_worker_count; // 每个 worker 的分配次数
    assert(one_worker_count > group_length); // 先检查，避免 unsigned 前缀长度下溢。
    const std::size_t prefix_length = one_worker_count - group_length;    // 全部 worker 之前的分配
    assert(prefix_length > 0 && group_length > 0);
    // 核对结构：单 worker 序列是前缀加一组，双 worker 序列是同一前缀加两组相同分配。
    for (std::size_t index = 0; index < group_length; ++index) {
        assert(one_worker_sizes[prefix_length + index] == two_worker_sizes[prefix_length + index]);
        assert(one_worker_sizes[prefix_length + index]
               == two_worker_sizes[prefix_length + group_length + index]);
    }
    // 组的首次分配是 work_thread 对象：该序号用于核对首个 worker 对象被释放。
    assert(one_worker_sizes[prefix_length] == sizeof(work_thread));

    // 注入点取第 2 个 worker 组的首次分配，也就是第二个 work_thread 对象。
    // 能走到该序号，说明第一个 worker 组已全部分配成功，此前创建的线程真实存在。
    worker_allocation_count = 0;
    worker_allocation_ordinal = prefix_length + group_length + 1;
    first_worker_object_ordinal = prefix_length + 1;
    first_worker_object = nullptr;
    first_worker_released = false;
    fail_worker_allocation = true;
    bool failed = false;
    try { thread_pool pool(4); }
    catch (const std::bad_alloc&) { failed = true; }
    fail_worker_allocation = false;
    assert(failed);
    assert(worker_allocation_count == worker_allocation_ordinal); // 注入发生在推导出的位置。
    assert(first_worker_object != nullptr && first_worker_released); // 首个 worker 已创建并回收。
    first_worker_object_ordinal = 0;
    first_worker_object = nullptr;

    // 失败回收后仍能创建并使用新池。
    const int exits_before = registered_worker_exits();
    thread_pool retry(1);
    auto result = retry.submit([] {
        worker_exit_touch touch;
        return 9;
    });
    assert(result.get() == 9);
    retry.shutdown();
    check_terminated(retry, exits_before + 1);
}

// 未显式关闭时析构排空队列。
void destruction_without_close_drains_queue() {
    constexpr int worker_count = 4;
    constexpr int queued_tasks = 12;
    const int exits_before = registered_worker_exits();
    std::array<std::atomic<int>, queued_tasks> calls{};
    auto pool = std::make_unique<thread_pool>(worker_count);

    // 每个 worker 领取一个阻塞任务并登记退出记录，共同占住全部 worker。
    std::latch entered(worker_count), release(1);
    std::vector<std::future<void>> claimed;
    for (int index = 0; index < worker_count; ++index) {
        claimed.push_back(pool->submit([&] {
            worker_exit_touch touch;
            entered.count_down();
            release.wait();
        }));
    }
    entered.wait();
    // 后续任务只能排队，析构时必须全部执行。
    std::vector<std::future<int>> queued;
    for (int index = 0; index < queued_tasks; ++index) {
        queued.push_back(pool->submit([&, index] {
            ++calls[index];
            return index;
        }));
    }

    std::promise<void> destroyed;
    auto finished = destroyed.get_future();
    auto* observed_pool = pool.get();
    std::jthread owner([&] {
        pool.reset(); // 未显式关闭：析构须排空队列并等待全部 worker。
        destroyed.set_value();
    });
    // 测试专用只读观察：全部 worker 被占住，析构尚不能结束，同步成员仍存活。
    // 确认析构已选择优雅关闭且队列仍有任务后，停止观察再放行 worker。
    thread_pool_test_access::wait_for_closing(*observed_pool, status_t::SHUTDOWN);
    assert(thread_pool_test_access::queued_count(*observed_pool) == queued_tasks);
    for (auto& task : queued) {
        assert(task.wait_for(0s) == std::future_status::timeout);
    }
    assert(finished.wait_for(0s) == std::future_status::timeout); // 阻塞任务未放行。
    release.count_down();
    finished.get();
    owner.join();
    for (auto& task : claimed) {
        assert(task.wait_for(0s) == std::future_status::ready);
        task.get();
    }
    for (int index = 0; index < queued_tasks; ++index) {
        assert(queued[index].wait_for(0s) == std::future_status::ready);
        assert(queued[index].get() == index);
        assert(calls[index].load() == 1);
    }
    assert(registered_worker_exits() == exits_before + worker_count);
}

// 显式关闭完成后析构不重复回收。
void destruction_after_explicit_close_does_not_reclaim_again() {
    constexpr int worker_count = 3;
    int exits_before = registered_worker_exits();

    // 显式优雅关闭后再析构：worker 集合已空，析构复查 TERMINATED 后返回。
    {
        thread_pool pool(worker_count);
        all_workers_run(pool, worker_count);
        pool.shutdown();
        assert(registered_worker_exits() == exits_before + worker_count);
        assert(thread_pool_test_access::state(pool) == status_t::TERMINATED);
        assert(thread_pool_test_access::owner_cleared(pool));
        assert(thread_pool_test_access::worker_count(pool) == 0);
    }
    assert(registered_worker_exits() == exits_before + worker_count);
    exits_before += worker_count;

    // 显式立即关闭后再析构同样不重复回收。
    {
        thread_pool pool(worker_count);
        all_workers_run(pool, worker_count);
        pool.shutdown_now();
        assert(thread_pool_test_access::worker_count(pool) == 0);
    }
    assert(registered_worker_exits() == exits_before + worker_count);
    exits_before += worker_count;

    // 完成后重复关闭：计数不变，多个调用者都不再回收。
    {
        thread_pool pool(worker_count);
        all_workers_run(pool, worker_count);
        pool.shutdown();
        for (int attempt = 0; attempt < 32; ++attempt) {
            pool.shutdown();
            pool.shutdown_now();
        }
        assert(registered_worker_exits() == exits_before + worker_count);
        check_terminated(pool, exits_before + worker_count);
    }
    exits_before += worker_count;
    assert(registered_worker_exits() == exits_before);

    // 未显式关闭的析构：走优雅关闭回收一次。
    {
        thread_pool pool(worker_count);
        all_workers_run(pool, worker_count);
    }
    assert(registered_worker_exits() == exits_before + worker_count);
}

// 入队分配失败时已有队列任务保持不变。
void enqueue_allocation_failure_keeps_queued_tasks() {
    constexpr int queued_tasks = 6;
    constexpr int measurement_samples = 128;
    constexpr int allocation_budget = 1024;
    constexpr int growth_attempts = 2048;
    fail_enqueue_allocation = false;
    enqueue_allocations_before_failure = 0;

    thread_pool pool(1);
    std::latch entered(1), release(1);
    std::array<std::atomic<int>, queued_tasks> calls{};
    std::vector<int> order;
    order.reserve(queued_tasks);
    // 唯一 worker 被阻塞任务占住：注入期间不会有 worker 领取，队列规模稳定。
    auto claimed = pool.submit([&] {
        worker_exit_touch touch;
        entered.count_down();
        release.wait();
    });
    entered.wait();
    std::vector<std::future<int>> queued;
    queued.reserve(queued_tasks);
    for (int index = 0; index < queued_tasks; ++index) {
        queued.push_back(pool.submit([&, index] {
            ++calls[index];
            order.push_back(index);
            return index;
        }));
    }
    assert(thread_pool_test_access::queued_count(pool) == queued_tasks); // 已有任务全部在队列中。
    for (auto& result : queued) {
        assert(result.wait_for(0s) == std::future_status::timeout);
    }

    // 量出准备阶段（不含队列扩容）的分配数：成功入队多次并取最小值。
    // 容器扩容留在注入关闭后进行，避免把测试自身的分配计入被测提交。
    auto sentinel = [] { return 5; };
    std::size_t preparation_allocations = 0;
    std::vector<std::future<int>> accepted;
    accepted.reserve(measurement_samples);
    for (int sample = 0; sample < measurement_samples; ++sample) {
        std::future<int> result;
        fail_enqueue_allocation = true;
        enqueue_allocations_before_failure = allocation_budget;
        result = pool.submit(sentinel);
        const auto consumed =
            static_cast<std::size_t>(allocation_budget - enqueue_allocations_before_failure);
        fail_enqueue_allocation = false;
        assert(enqueue_allocations_before_failure > 0); // 本次提交未触发注入。
        if (sample == 0 || consumed < preparation_allocations) {
            preparation_allocations = consumed;
        }
        accepted.push_back(std::move(result));
    }
    assert(preparation_allocations > 0);

    // 放行准备阶段的分配，让紧随其后的队列扩容分配失败；队列需要增长时该分配必然发生。
    bool allocation_failed = false;
    std::vector<std::future<int>> grown;
    grown.reserve(growth_attempts);
    for (int attempt = 0; attempt < growth_attempts && !allocation_failed; ++attempt) {
        const auto previous_size = thread_pool_test_access::queued_count(pool);
        std::future<int> result;
        bool failed_this_attempt = false;
        fail_enqueue_allocation = true;
        enqueue_allocations_before_failure = static_cast<int>(preparation_allocations);
        try {
            result = pool.submit(sentinel);
        } catch (const std::bad_alloc&) {
            failed_this_attempt = true;
            assert(!fail_enqueue_allocation); // 异常来自已启用的注入。
        }
        fail_enqueue_allocation = false;
        if (!failed_this_attempt) {
            grown.push_back(std::move(result));
            continue;
        }
        allocation_failed = true;
        // 失败未改变池状态、负责人身份、worker 集合和已有队列任务。
        assert(thread_pool_test_access::state(pool) == status_t::RUNNING);
        assert(thread_pool_test_access::owner_cleared(pool));
        assert(thread_pool_test_access::worker_count(pool) == 1);
        assert(thread_pool_test_access::queued_count(pool) == previous_size);
        for (auto& pending : queued) {
            assert(pending.wait_for(0s) == std::future_status::timeout);
        }
        for (int index = 0; index < queued_tasks; ++index) {
            assert(calls[index].load() == 0);
        }
    }
    assert(allocation_failed);
    auto retried = pool.submit(sentinel); // 关闭前即可再次成功提交，不只核对关闭可重试。

    // 已有任务与已接受任务仍按原顺序各执行一次，随后可重试关闭并排空队列。
    release.count_down();
    pool.shutdown();
    claimed.get();
    for (int index = 0; index < queued_tasks; ++index) {
        assert(queued[index].get() == index);
        assert(calls[index].load() == 1);
    }
    for (auto& result : accepted) { assert(result.get() == 5); }
    for (auto& result : grown) { assert(result.get() == 5); }
    assert(retried.get() == 5);
    assert(order == (std::vector<int>{0, 1, 2, 3, 4, 5}));
    check_terminated(pool, 1);
}

// 用户捕获对象销毁在队列锁外进行。
void user_captures_destroyed_outside_queue_lock() {
    std::atomic<int> executed_destroyed{0};
    std::atomic<int> discarded_destroyed{0};
    std::promise<std::thread::id> executed_signal, discarded_signal;
    auto executed_ready = executed_signal.get_future();
    auto discarded_ready = discarded_signal.get_future();
    std::latch claimed_entered(1), claimed_release(1);
    std::latch executing_entered(1), executing_release(1);
    std::thread::id executing_thread, closing_thread;

    thread_pool pool(1);
    // 唯一 worker 先被占住，随后提交的两个任务只能排队，领取顺序因此确定。
    auto claimed = pool.submit([&] {
        worker_exit_touch touch;
        claimed_entered.count_down();
        claimed_release.wait();
    });
    claimed_entered.wait();

    // 将被领取的任务：放弃 future，让 Task 的共享状态持有最后一个捕获引用；
    // worker 执行后于锁外销毁 Task，捕获对象随之在该 worker 线程上销毁。
    auto executed_probe = std::unique_ptr<lock_probe>(
        new lock_probe{pool, executed_destroyed, executed_signal});
    (void)pool.submit([&, probe = std::move(executed_probe)] {
        worker_exit_touch touch;
        executing_thread = std::this_thread::get_id();
        executing_entered.count_down();
        executing_release.wait();
    });
    // 未领取任务：放弃 future 后由关闭线程在锁外销毁丢弃队列，捕获对象随之销毁。
    auto discarded_probe = std::unique_ptr<lock_probe>(
        new lock_probe{pool, discarded_destroyed, discarded_signal});
    (void)pool.submit([probe = std::move(discarded_probe)] {
        assert(false); // 丢弃的任务不应被调用。
    });

    // 放行首个阻塞任务，让 worker 领取第二个任务并阻塞其中，丢弃任务留在队列。
    claimed_release.count_down();
    executing_entered.wait();

    std::jthread closing([&] {
        closing_thread = std::this_thread::get_id();
        pool.shutdown_now();
    });
    // 丢弃发生在关闭返回之前，且此刻已领取任务仍持有其捕获对象，两条销毁路径可区分。
    assert(discarded_ready.wait_for(5s) == std::future_status::ready);
    assert(discarded_ready.get() == closing_thread);
    assert(discarded_destroyed.load() == 1);
    assert(executed_destroyed.load() == 0);
    assert(executed_ready.wait_for(0s) == std::future_status::timeout);

    executing_release.count_down();
    claimed.get();
    closing.join(); // 已领取任务结束后 worker 退出，其捕获对象已在锁外销毁。
    assert(executed_ready.wait_for(0s) == std::future_status::ready);
    assert(executed_ready.get() == executing_thread);
    assert(executed_destroyed.load() == 1);
    assert(discarded_destroyed.load() == 1);
    check_terminated(pool, 1);
}

void worker_cannot_close_own_pool() {
    thread_pool pool(1);
    auto graceful = pool.submit([&] {
        worker_exit_touch touch;
        pool.shutdown();
    });
    auto immediate = pool.submit([&] {
        worker_exit_touch touch;
        pool.shutdown_now();
    });
    auto direct = pool.submit([&] {
        worker_exit_touch touch;
        thread_pool_test_access::stop(pool, status_t::SHUTDOWN);
    });
    for (auto* result : {&graceful, &immediate, &direct}) {
        bool rejected = false;
        try { result->get(); }
        catch (const std::logic_error&) { rejected = true; }
        assert(rejected);
    }
    assert(thread_pool_test_access::state(pool) == status_t::RUNNING);
    assert(pool.submit([] {
               worker_exit_touch touch;
               return 7;
           }).get() == 7);
    pool.shutdown();
    check_terminated(pool, 1);
}

void concurrent_close_keeps_first_mode(status_t first_mode) {
    constexpr int waiter_count = 8;
    constexpr int task_count = 8;
    thread_pool pool(1);
    std::latch entered(1), release(1), ready(waiter_count), start(1);
    std::array<std::atomic<int>, task_count> calls{};
    auto claimed = pool.submit([&] {
        worker_exit_touch touch;
        entered.count_down();
        release.wait();
        return 42;
    });
    entered.wait();
    std::vector<std::future<int>> queued;
    for (int index = 0; index < task_count; ++index) {
        queued.push_back(pool.submit([&, index] { ++calls[index]; return index; }));
    }
    std::promise<void> owner_done;
    auto owner_result = owner_done.get_future();
    std::jthread owner([&] {
        if (first_mode == status_t::SHUTDOWN) { pool.shutdown(); }
        else { pool.shutdown_now(); }
        owner_done.set_value();
    });
    thread_pool_test_access::wait_for_closing(pool, first_mode);
    bool rejected = false;
    try { (void)pool.submit([] {}); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);

    std::array<std::promise<void>, waiter_count> done;
    std::array<std::future<void>, waiter_count> results;
    std::vector<std::jthread> waiters;
    for (int index = 0; index < waiter_count; ++index) {
        results[index] = done[index].get_future();
        waiters.emplace_back([&, index] {
            ready.count_down();
            start.wait();
            if (index % 2 == 0) { pool.shutdown(); }
            else { pool.shutdown_now(); }
            done[index].set_value();
        });
    }
    ready.wait();
    start.count_down();
    if (first_mode == status_t::SHUTDOWN_NOW) {
        for (auto& result : queued) { check_broken_promise(result); }
    } else {
        for (auto& result : queued) {
            assert(result.wait_for(0s) == std::future_status::timeout);
        }
    }
    assert(owner_result.wait_for(0s) == std::future_status::timeout);
    for (auto& result : results) {
        assert(result.wait_for(0s) == std::future_status::timeout);
    }
    assert(thread_pool_test_access::state(pool) == first_mode);
    release.count_down();
    owner_result.get();
    for (auto& result : results) { result.get(); }
    owner.join();
    waiters.clear();
    assert(claimed.get() == 42);
    for (int index = 0; index < task_count; ++index) {
        if (first_mode == status_t::SHUTDOWN) { assert(queued[index].get() == index); }
        assert(calls[index].load() == (first_mode == status_t::SHUTDOWN ? 1 : 0));
    }
    check_terminated(pool, 1);
}

struct close_on_destroy {
    thread_pool& pool;
    thread_pool& other;
    int& rejected;
    std::latch& destroyed;
    ~close_on_destroy() {
        try { pool.shutdown(); }
        catch (const std::logic_error&) { ++rejected; }
        try { pool.shutdown_now(); }
        catch (const std::logic_error&) { ++rejected; }
        try { (void)pool.submit([] {}); }
        catch (const std::runtime_error&) { ++rejected; }
        other.shutdown(); // 同一关闭线程可关闭其他池。
        destroyed.count_down();
    }
};

void discarded_capture_reentry_is_rejected() {
    thread_pool pool(1), other(1);
    std::latch entered(1), release(1), destroyed(1);
    auto claimed = pool.submit([&] {
        worker_exit_touch touch;
        entered.count_down();
        release.wait();
    });
    entered.wait();
    int rejected = 0;
    auto probe = std::unique_ptr<close_on_destroy>(
        new close_on_destroy{pool, other, rejected, destroyed});
    // 放弃 future，使待丢弃 Task 持有最后一个共享状态引用。
    (void)pool.submit([owned = std::move(probe)] { assert(false); });
    std::jthread owner([&] { pool.shutdown_now(); });
    destroyed.wait();
    assert(rejected == 3);
    // other 的 worker 此时仍在运行任务，退出计数在释放并 join 之后再核对。
    assert(thread_pool_test_access::state(pool) == status_t::SHUTDOWN_NOW);
    release.count_down();
    claimed.get();
    owner.join();
    check_terminated(other, 1);
    check_terminated(pool, 1);
}

void shutdown_preparation_failure_leaves_pool_running() {
    thread_pool pool(1);
    std::latch entered(1), release(1);
    auto claimed = pool.submit([&] {
        worker_exit_touch touch;
        entered.count_down();
        release.wait();
    });
    entered.wait();
    auto queued = pool.submit([] { return 23; });
    fail_shutdown_allocation = true;
    bool failed = false;
    try { pool.shutdown_now(); }
    catch (const std::bad_alloc&) { failed = true; }
    assert(failed && !fail_shutdown_allocation);
    assert(thread_pool_test_access::state(pool) == status_t::RUNNING);
    assert(thread_pool_test_access::owner_cleared(pool));
    auto retry = pool.submit([] { return 24; });
    release.count_down();
    pool.shutdown();
    claimed.get();
    assert(queued.get() == 23 && retry.get() == 24);
    check_terminated(pool, 1);
}

struct test_case {
    std::string_view name;
    void (*run)();
};
constexpr std::array cases{
    test_case{"worker_count", requested_and_default_worker_count},
    test_case{"zero_worker_count", zero_worker_count_is_rejected},
    test_case{"partial_construction_failure", partial_construction_failure_reclaims_workers},
    test_case{"destruction_drains", destruction_without_close_drains_queue},
    test_case{"destruction_after_close", destruction_after_explicit_close_does_not_reclaim_again},
    test_case{"enqueue_allocation_failure", enqueue_allocation_failure_keeps_queued_tasks},
    test_case{"capture_destroyed_outside_lock", user_captures_destroyed_outside_queue_lock},
    test_case{"worker_self_close", worker_cannot_close_own_pool},
    test_case{"concurrent_close_graceful",
              [] { concurrent_close_keeps_first_mode(status_t::SHUTDOWN); }},
    test_case{"concurrent_close_immediate",
              [] { concurrent_close_keeps_first_mode(status_t::SHUTDOWN_NOW); }},
    test_case{"discarded_capture_reentry", discarded_capture_reentry_is_rejected},
    test_case{"shutdown_preparation_failure", shutdown_preparation_failure_leaves_pool_running}
};
}

int main(int argc, char* argv[]) {
    if (argc == 2) {
        for (const auto& test : cases) {
            if (test.name == argv[1]) {
                test.run();
                return 0;
            }
        }
    }
    std::cerr << "Expected one lifecycle test case name\n";
    return 1;
}
