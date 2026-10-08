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
#include <latch>
#include <new>

namespace {
// 本机 UCRT64 测试：第二次 work_thread 对象分配失败，首个 worker 已创建。
// pool(4) 的 vector 预留、队列和 thread callable 分配大小均与 work_thread 不同。
thread_local bool fail_worker_allocation = false;
thread_local int worker_allocations = 0;
thread_local void* first_worker_allocation = nullptr;
thread_local bool first_worker_released = false;
thread_local bool fail_shutdown_allocation = false;
}

void* operator new(std::size_t size) {
    if (fail_shutdown_allocation) {
        fail_shutdown_allocation = false;
        throw std::bad_alloc{};
    }
    if (fail_worker_allocation && size == sizeof(work_thread)) {
        if (++worker_allocations == 2) {
            fail_worker_allocation = false;
            throw std::bad_alloc{};
        }
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        if (fail_worker_allocation && size == sizeof(work_thread)) {
            first_worker_allocation = memory;
        }
        return memory;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept {
    if (memory != nullptr && memory == first_worker_allocation) {
        first_worker_released = true;
    }
    std::free(memory);
}
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

struct thread_pool_test_access {
    static std::size_t worker_count(const thread_pool& pool) {
        return pool.worker_threads_.size();
    }
    static status_t state(thread_pool& pool) {
        std::lock_guard lock(pool.queue_mutex_);
        return pool.state_;
    }
    static void wait_for_closing(thread_pool& pool, status_t expected) {
        std::unique_lock lock(pool.queue_mutex_);
        pool.task_ready_.wait(lock, [&] { return pool.state_ != status_t::RUNNING; });
        assert(pool.state_ == expected);
    }
    static bool owner_cleared(thread_pool& pool) {
        std::lock_guard lock(pool.queue_mutex_);
        return pool.closing_thread_id_ == std::thread::id{};
    }
    static void stop(thread_pool& pool, status_t requested) { pool.stop(requested); }
};

namespace {
void check_terminated(thread_pool& pool) {
    assert(thread_pool_test_access::state(pool) == status_t::TERMINATED);
    assert(thread_pool_test_access::owner_cleared(pool));
    assert(thread_pool_test_access::worker_count(pool) == 0);
    pool.shutdown();
    pool.shutdown_now(); // 完成后重复关闭不再回收，也不准备丢弃队列。
}

void check_broken_promise(std::future<int>& result) {
    bool broken = false;
    try { (void)result.get(); }
    catch (const std::future_error& error) {
        broken = error.code() == std::make_error_code(std::future_errc::broken_promise);
    }
    assert(broken);
}

void worker_cannot_close_own_pool() {
    thread_pool pool(1);
    auto graceful = pool.submit([&] { pool.shutdown(); });
    auto immediate = pool.submit([&] { pool.shutdown_now(); });
    auto direct = pool.submit([&] {
        thread_pool_test_access::stop(pool, status_t::SHUTDOWN);
    });
    for (auto* result : {&graceful, &immediate, &direct}) {
        bool rejected = false;
        try { result->get(); }
        catch (const std::logic_error&) { rejected = true; }
        assert(rejected);
    }
    assert(thread_pool_test_access::state(pool) == status_t::RUNNING);
    assert(pool.submit([] { return 7; }).get() == 7);
    pool.shutdown();
    check_terminated(pool);
}

void concurrent_close_keeps_first_mode(status_t first_mode) {
    constexpr int waiter_count = 8;
    constexpr int task_count = 8;
    thread_pool pool(1);
    std::latch entered(1), release(1), ready(waiter_count), start(1);
    std::array<std::atomic<int>, task_count> calls{};
    auto claimed = pool.submit([&] { entered.count_down(); release.wait(); return 42; });
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
            assert(result.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        }
    }
    assert(owner_result.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    for (auto& result : results) {
        assert(result.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
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
    check_terminated(pool);
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
    auto claimed = pool.submit([&] { entered.count_down(); release.wait(); });
    entered.wait();
    int rejected = 0;
    auto probe = std::unique_ptr<close_on_destroy>(
        new close_on_destroy{pool, other, rejected, destroyed});
    // 放弃 future，使待丢弃 Task 持有最后一个共享状态引用。
    (void)pool.submit([owned = std::move(probe)] { assert(false); });
    std::jthread owner([&] { pool.shutdown_now(); });
    destroyed.wait();
    assert(rejected == 3);
    check_terminated(other);
    assert(thread_pool_test_access::state(pool) == status_t::SHUTDOWN_NOW);
    release.count_down();
    claimed.get();
    owner.join();
    check_terminated(pool);
}

void shutdown_preparation_failure_leaves_pool_running() {
    thread_pool pool(1);
    std::latch entered(1), release(1);
    auto claimed = pool.submit([&] { entered.count_down(); release.wait(); });
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
    check_terminated(pool);
}

void construct_requested_and_default_workers() {
    {
        thread_pool pool(3);
        assert(thread_pool_test_access::worker_count(pool) == 3);
        assert(pool.submit([] { return 42; }).get() == 42);
    }
    {
        const auto hardware_count = std::thread::hardware_concurrency();
        thread_pool pool;
        assert(thread_pool_test_access::worker_count(pool)
               == (hardware_count == 0 ? 1 : hardware_count));
        assert(pool.submit([] { return 17; }).get() == 17);
    }
    bool rejected = false;
    try { thread_pool pool(0); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

void destruction_drains_and_waits() {
    constexpr int queued_tasks = 8;
    std::latch entered(1), release(1), destroying(1);
    std::array<std::atomic<int>, queued_tasks + 1> calls{};
    std::vector<std::future<int>> results;
    auto pool = std::make_unique<thread_pool>(1);
    results.push_back(pool->submit([&] {
        entered.count_down();
        release.wait();
        ++calls[0];
        return 0;
    }));
    entered.wait();
    for (int index = 1; index <= queued_tasks; ++index) {
        results.push_back(pool->submit([&, index] {
            ++calls[index];
            return index;
        }));
    }
    std::promise<void> destroyed;
    auto finished = destroyed.get_future();
    std::jthread owner([&] {
        destroying.count_down();
        pool.reset();
        destroyed.set_value();
    });
    destroying.wait();
    assert(finished.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    release.count_down();
    finished.get();
    owner.join();
    for (int index = 0; index <= queued_tasks; ++index) {
        assert(results[index].wait_for(std::chrono::seconds(0)) == std::future_status::ready);
        assert(results[index].get() == index);
        assert(calls[index].load() == 1);
    }
}

void partial_construction_failure_reclaims_workers() {
    fail_worker_allocation = true;
    worker_allocations = 0;
    first_worker_allocation = nullptr;
    first_worker_released = false;
    bool failed = false;
    try { thread_pool pool(4); }
    catch (const std::bad_alloc&) { failed = true; }
    fail_worker_allocation = false;
    assert(failed && worker_allocations == 2 && first_worker_released);
    first_worker_allocation = nullptr;
    // 失败回收后仍能创建并使用新池。
    thread_pool retry(1);
    assert(retry.submit([] { return 9; }).get() == 9);
}
}

int main() {
    worker_cannot_close_own_pool();
    concurrent_close_keeps_first_mode(status_t::SHUTDOWN);
    concurrent_close_keeps_first_mode(status_t::SHUTDOWN_NOW);
    discarded_capture_reentry_is_rejected();
    shutdown_preparation_failure_leaves_pool_running();
    construct_requested_and_default_workers();
    destruction_drains_and_waits();
    partial_construction_failure_reclaims_workers();
}
