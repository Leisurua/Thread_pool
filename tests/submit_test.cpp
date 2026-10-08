// 行为断言在所有构建配置中启用。
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "thread_pool.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <latch>
#include <new>
#include <string>

namespace {
// 先测量无队列扩容时 submit 的分配数，再仅对额外的队列分配注入失败。
thread_local bool count_allocations = false;
thread_local std::size_t allocation_count = 0;
thread_local std::size_t allocations_before_failure = 0;
thread_local bool fail_queue_allocation = false;

struct user_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct copy_failure {
    copy_failure() = default;
    copy_failure(const copy_failure&) { throw user_error("copy failure"); }
    int operator()() & { return 0; }
};

struct storage_failure {
    storage_failure() = default;
    storage_failure(const storage_failure&) = delete;
    storage_failure(storage_failure&&) { throw std::runtime_error("storage failure"); }
};

struct left_only {
    int operator()() & { return 17; }
    int operator()() && = delete;
};

struct slow_copy {
    std::latch& entered;
    std::latch& release;
    slow_copy(std::latch& begin, std::latch& end) : entered(begin), release(end) {}
    slow_copy(const slow_copy& other) : entered(other.entered), release(other.release) {
        entered.count_down();
        release.wait();
    }
    slow_copy(slow_copy&& other) noexcept : entered(other.entered), release(other.release) {}
    int operator()() & { return 2; }
};

struct submit_on_rejected_cleanup {
    thread_pool& pool;
    int& cleanups;
    ~submit_on_rejected_cleanup() {
        // 被拒绝任务的捕获在锁外释放，否则再次进入 enqueue 会死锁。
        try { (void)pool.submit([] {}); }
        catch (const std::runtime_error&) { ++cleanups; }
    }
};
}

void* operator new(std::size_t size) {
    if (count_allocations) {
        ++allocation_count;
    }
    if (fail_queue_allocation) {
        if (allocations_before_failure == 0) {
            fail_queue_allocation = false;
            throw std::bad_alloc{};
        }
        --allocations_before_failure;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

struct thread_pool_test_access {
    static void exercise_submit(thread_pool& pool);
};

void thread_pool_test_access::exercise_submit(thread_pool& pool) {
    // 使用产品构造/析构；先退出并回收 worker，再通过状态桩同步驱动 submit/Task。
    pool.shutdown();
    pool.state_ = status_t::RUNNING;

    auto take = [&] {
        std::lock_guard lock(pool.queue_mutex_);
        assert(!pool.tasks_.empty());
        auto task = std::move(pool.tasks_.front());
        pool.tasks_.pop();
        return task;
    };
    auto execute_one = [&] {
        auto task = take();
        task(); // 必须在锁外执行，用户异常通过 future 反馈。
    };
    auto drain = [&] {
        while (!pool.tasks_.empty()) {
            execute_one();
        }
    };

    // 内部 enqueue 拒绝已移动的空 Task，有效任务仍可入队并取得原 future 结果。
    std::packaged_task<int()> operation([] { return 19; });
    auto queued_result = operation.get_future();
    thread_pool::Task empty(std::move(operation));
    auto valid = std::move(empty);
    assert(!empty.valid() && valid.valid());
    bool invalid_rejected = false;
    try { pool.enqueue(std::move(empty)); }
    catch (const std::invalid_argument&) { invalid_rejected = true; }
    assert(invalid_rejected && pool.tasks_.empty());
    assert(queued_result.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    pool.enqueue(std::move(valid));
    execute_one();
    assert(queued_result.get() == 19);

    // 返回类型混合入队；submit 返回时没有 worker，因此 future 必须仍未就绪。
    int input = 5;
    int target = 20;
    auto value = pool.submit([](int x) { return x * 2; }, input);
    input = 100;
    auto nothing = pool.submit([](int& x) { ++x; }, std::ref(target));
    auto reference = pool.submit([](int& x) -> int& { return x; }, std::ref(target));
    auto movable = pool.submit(
        [offset = std::make_unique<int>(3)](std::unique_ptr<int> x) { return *offset + *x; },
        std::make_unique<int>(4));
    auto left = pool.submit(left_only{});
    assert(value.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    assert(pool.tasks_.size() == 5);
    drain();
    assert(value.get() == 10 && target == 21);
    nothing.get();
    assert(&reference.get() == &target);
    assert(movable.get() == 7 && left.get() == 17);

    // 原函数异常的动态类型与信息直接保留；后续任务继续执行。
    auto failed = pool.submit([]() -> int { throw user_error("call failure"); });
    auto after_failure = pool.submit([] { return 42; });
    drain();
    bool original_error = false;
    try { (void)failed.get(); }
    catch (const user_error& error) {
        original_error = std::string(error.what()) == "call failure";
    }
    assert(original_error && after_failure.get() == 42);

    // packaged_task 内部存储结果时抛出的异常保持原类型。
    auto bad_storage = pool.submit([] { return storage_failure{}; });
    execute_one();
    bool original_storage_error = false;
    try { (void)bad_storage.get(); }
    catch (const std::runtime_error& error) {
        original_storage_error = std::string(error.what()) == "storage failure";
    }
    assert(original_storage_error);

    // 销毁未执行 Task，值/void/引用结果均应反馈 broken_promise，不运行 callable。
    int discarded_calls = 0;
    auto discarded_value = pool.submit([&] { ++discarded_calls; return 1; });
    auto discarded_void = pool.submit([&] { ++discarded_calls; });
    auto discarded_reference = pool.submit([&]() -> int& { ++discarded_calls; return target; });
    for (int index = 0; index < 3; ++index) {
        auto task = take(); // 作用域结束时在锁外销毁，不执行任务。
    }
    auto check_broken_promise = [](auto& result) {
        assert(result.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
        bool reported = false;
        try { (void)result.get(); }
        catch (const std::future_error& error) {
            reported = error.code() == std::make_error_code(std::future_errc::broken_promise);
        }
        assert(reported);
    };
    check_broken_promise(discarded_value);
    check_broken_promise(discarded_void);
    check_broken_promise(discarded_reference);
    assert(discarded_calls == 0);

    // 仅 RUNNING 接受提交；三种关闭状态均拒绝，不入队。
    for (auto state : {status_t::SHUTDOWN, status_t::SHUTDOWN_NOW, status_t::TERMINATED}) {
        pool.state_ = state;
        bool rejected = false;
        try { (void)pool.submit([] { return 1; }); }
        catch (const std::runtime_error& error) {
            rejected = std::string(error.what()) == "thread pool submission rejected";
        }
        assert(rejected && pool.tasks_.empty());
    }

    int cleanups = 0;
    auto probe = std::unique_ptr<submit_on_rejected_cleanup>(
        new submit_on_rejected_cleanup{pool, cleanups});
    bool cleanup_submission_rejected = false;
    try { (void)pool.submit([owned = std::move(probe)] { assert(false); }); }
    catch (const std::runtime_error&) { cleanup_submission_rejected = true; }
    assert(cleanup_submission_rejected && cleanups == 1 && pool.tasks_.empty());
    pool.state_ = status_t::RUNNING;

    // 用户复制构造抛出的 runtime_error 派生异常保持类型与信息。
    copy_failure callable;
    original_error = false;
    try { (void)pool.submit(callable); }
    catch (const user_error& error) {
        original_error = std::string(error.what()) == "copy failure";
    }
    assert(original_error && pool.tasks_.empty());

    // 受控复制尚未完成时，另一个 submit 可先入队并执行，证明准备不占队列锁。
    std::latch entered(1), release(1);
    slow_copy slow(entered, release);
    std::future<int> slow_result;
    std::jthread submitting([&] { slow_result = pool.submit(slow); });
    entered.wait();
    auto fast_result = pool.submit([] { return 1; });
    assert(pool.tasks_.size() == 1);
    execute_one();
    assert(fast_result.get() == 1);
    release.count_down();
    submitting.join();
    assert(pool.tasks_.size() == 1);
    drain();
    assert(slow_result.get() == 2);

    // 多线程提交；逐任务记录调用次数，验证所有任务恰好执行一次。
    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = 24;
    std::vector<std::future<int>> results[producer_count];
    std::vector<std::jthread> producers;
    std::vector<int> calls(producer_count * tasks_per_producer, 0);
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = 0; index < tasks_per_producer; ++index) {
                const int expected = producer * tasks_per_producer + index;
                results[producer].push_back(pool.submit([&, expected] {
                    ++calls[expected];
                    return expected;
                }));
            }
        });
    }
    producers.clear(); // jthread 析构等待提交线程完成。
    assert(pool.tasks_.size() == producer_count * tasks_per_producer);
    drain();
    for (int producer = 0; producer < producer_count; ++producer) {
        for (int index = 0; index < tasks_per_producer; ++index) {
            const int expected = producer * tasks_per_producer + index;
            assert(results[producer][index].get() == expected);
            assert(calls[expected] == 1);
        }
    }

    // 同一 callable 多次提交并测量最小分配数，排除偶发的 deque 扩容分配。
    // 此后跳过这些准备分配，只使队列扩容失败，不依赖产品进度记录或对象大小。
    auto allocation_task = [] { return 3; };
    std::size_t preparation_allocations = 0;
    for (int sample = 0; sample < 128; ++sample) {
        allocation_count = 0;
        count_allocations = true;
        auto result = pool.submit(allocation_task);
        count_allocations = false;
        if (sample == 0 || allocation_count < preparation_allocations) {
            preparation_allocations = allocation_count;
        }
        execute_one();
        assert(result.get() == 3);
    }
    assert(preparation_allocations > 0);

    // 队列扩容失败不改变已有任务，异常直接传播且可重试。
    std::vector<std::future<int>> before_allocation_failure;
    before_allocation_failure.reserve(2048);
    bool allocation_failed = false;
    for (int attempt = 0; attempt < 2048 && !allocation_failed; ++attempt) {
        const auto previous_size = pool.tasks_.size();
        allocations_before_failure = preparation_allocations;
        fail_queue_allocation = true;
        try {
            before_allocation_failure.push_back(pool.submit(allocation_task));
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
            assert(pool.tasks_.size() == previous_size);
            assert(!fail_queue_allocation); // 确认异常来自已启用的分配注入。
        }
        fail_queue_allocation = false;
    }
    assert(allocation_failed);
    auto retry = pool.submit([] { return 4; });
    drain();
    for (auto& result : before_allocation_failure) { assert(result.get() == 3); }
    assert(retry.get() == 4);

}

int main() {
    thread_pool pool(1);
    thread_pool_test_access::exercise_submit(pool);
}
