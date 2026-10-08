// 行为断言在所有构建配置中启用。
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "thread_pool.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <latch>
#include <string>

using namespace std::chrono_literals;

// 仅为未实现的池生命周期和公共状态接口提供测试驱动；worker/run 使用产品实现。
struct thread_pool_test_access {
    static void set_state(thread_pool& pool, status_t state) {
        {
            std::lock_guard lock(pool.mutex_);
            pool.state_ = state;
        }
        pool.task_ready_.notify_all();
    }

    static bool is_worker(const thread_pool& pool) {
        return pool.is_current_worker();
    }

    static std::size_t queued(thread_pool& pool) {
        std::lock_guard lock(pool.mutex_);
        return pool.tasks_.size();
    }

    static void join(thread_pool& pool) {
        pool.worker_threads_.clear();
    }

    static void discard_queued(thread_pool& pool) {
        std::queue<thread_pool::Task> discarded;
        {
            std::lock_guard lock(pool.mutex_);
            discarded.swap(pool.tasks_);
        }
        // 局部队列在主锁外销毁，future 反馈 broken_promise。
    }

    static void close_for_test(thread_pool& pool) {
        {
            std::lock_guard lock(pool.mutex_);
            if (pool.state_ == status_t::RUNNING || pool.state_ == status_t::PAUSED) {
                pool.state_ = status_t::SHUTDOWN;
            }
        }
        pool.task_ready_.notify_all();
        join(pool);
        discard_queued(pool);
    }
};

// 初始暂停让入队顺序和状态测试可控；这不是产品的构造/关闭实现。
thread_pool::thread_pool(std::size_t worker_count)
    : state_(status_t::PAUSED), join_in_progress_(false) {
    for (std::size_t index = 0; index < worker_count; ++index) {
        worker_threads_.push_back(std::make_unique<work_thread>(*this));
    }
}

thread_pool::~thread_pool() {
    thread_pool_test_access::close_for_test(*this);
}

namespace {
struct user_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void execution_and_identity() {
    thread_pool pool(1), other_pool(1);
    assert(!thread_pool_test_access::is_worker(pool));
    auto nested = pool.submit([&] {
        assert(thread_pool_test_access::is_worker(pool));
        assert(!thread_pool_test_access::is_worker(other_pool));
        return pool.submit([] { return 42; }); // 锁外执行，否则嵌套提交死锁。
    });
    auto failure = pool.submit([]() -> int { throw user_error("task failed"); });
    auto after_failure = pool.submit([] { return 17; });
    thread_pool_test_access::set_state(pool, status_t::RUNNING);
    auto inner = nested.get();
    assert(inner.get() == 42);
    bool original_error = false;
    try { (void)failure.get(); }
    catch (const user_error& error) {
        original_error = std::string(error.what()) == "task failed";
    }
    assert(original_error && after_failure.get() == 17);
}

void paused_and_fifo() {
    thread_pool pool(1);
    std::vector<int> order;
    std::vector<std::future<void>> results;
    for (int value = 1; value <= 3; ++value) {
        results.push_back(pool.submit([&, value] { order.push_back(value); }));
    }
    // 入队通知和一次无状态变化的唤醒都不能让暂停池领取任务。
    thread_pool_test_access::set_state(pool, status_t::PAUSED);
    assert(results.front().wait_for(50ms) == std::future_status::timeout);
    assert(thread_pool_test_access::queued(pool) == 3);
    thread_pool_test_access::set_state(pool, status_t::RUNNING);
    for (auto& result : results) { result.get(); }
    assert(order == (std::vector<int>{1, 2, 3}));
}

void out_of_order_completion() {
    thread_pool pool(2);
    std::latch entered(1), release(1);
    auto first = pool.submit([&] { entered.count_down(); release.wait(); });
    auto second = pool.submit([] { return 2; });
    thread_pool_test_access::set_state(pool, status_t::RUNNING);
    entered.wait();
    assert(second.get() == 2);
    assert(first.wait_for(0s) == std::future_status::timeout);
    release.count_down();
    first.get();
}

void concurrent_producers_execute_once() {
    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = 24;
    std::array<std::atomic<int>, producer_count * tasks_per_producer> calls{};
    std::array<std::vector<std::future<int>>, producer_count> results;
    std::latch ready(producer_count);
    thread_pool pool(4);
    thread_pool_test_access::set_state(pool, status_t::RUNNING);
    std::vector<std::jthread> producers;
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            ready.count_down();
            ready.wait();
            for (int index = 0; index < tasks_per_producer; ++index) {
                const int expected = producer * tasks_per_producer + index;
                results[producer].push_back(pool.submit([&, expected] {
                    calls[expected].fetch_add(1, std::memory_order_relaxed);
                    return expected;
                }));
            }
        });
    }
    producers.clear();
    for (int producer = 0; producer < producer_count; ++producer) {
        for (int index = 0; index < tasks_per_producer; ++index) {
            const int expected = producer * tasks_per_producer + index;
            assert(results[producer][index].get() == expected);
        }
    }
    thread_pool_test_access::set_state(pool, status_t::SHUTDOWN);
    thread_pool_test_access::join(pool);
    for (const auto& count : calls) {
        assert(count.load(std::memory_order_relaxed) == 1);
    }
    assert(thread_pool_test_access::queued(pool) == 0);
}

struct submit_on_destroy {
    thread_pool& pool;
    std::future<int>& result;
    std::latch& destroyed;
    ~submit_on_destroy() {
        result = pool.submit([] { return 9; });
        destroyed.count_down();
    }
};

void destruction_outside_lock() {
    thread_pool pool(1);
    std::latch entered(1), release(1), destroyed(1);
    std::future<int> destructor_result;
    auto probe = std::unique_ptr<submit_on_destroy>(
        new submit_on_destroy{pool, destructor_result, destroyed});
    auto result = pool.submit([owned = std::move(probe), &entered, &release] {
        entered.count_down();
        release.wait();
    });
    thread_pool_test_access::set_state(pool, status_t::RUNNING);
    entered.wait();
    result = {}; // 释放 future，保证 worker 的 Task 持有最后一个共享状态引用。
    release.count_down();
    destroyed.wait(); // 同步析构写入，future 就绪不能替代捕获对象销毁。
    assert(destructor_result.valid() && destructor_result.get() == 9);
}

void discarded_destruction_outside_lock() {
    thread_pool pool(1);
    std::latch destroyed(1);
    std::future<int> destructor_result;
    auto probe = std::unique_ptr<submit_on_destroy>(
        new submit_on_destroy{pool, destructor_result, destroyed});
    auto result = pool.submit([owned = std::move(probe)] { assert(false); });
    result = {}; // Task 持有最后一个共享状态引用，丢弃时确实触发用户析构。
    thread_pool_test_access::discard_queued(pool);
    destroyed.wait();
    assert(destructor_result.valid());
    thread_pool_test_access::set_state(pool, status_t::RUNNING);
    assert(destructor_result.get() == 9);
}

void graceful_shutdown_from_pause() {
    thread_pool pool(1);
    auto result = pool.submit([] { return 5; });
    thread_pool_test_access::set_state(pool, status_t::SHUTDOWN);
    thread_pool_test_access::join(pool);
    assert(result.get() == 5);
    assert(thread_pool_test_access::queued(pool) == 0);
}

void immediate_shutdown_preserves_claimed_task() {
    thread_pool pool(1);
    std::latch entered(1), release(1);
    auto first = pool.submit([&] { entered.count_down(); release.wait(); return 1; });
    auto second = pool.submit([] { assert(false); });
    auto third = pool.submit([] { assert(false); });
    thread_pool_test_access::set_state(pool, status_t::RUNNING);
    entered.wait();
    thread_pool_test_access::set_state(pool, status_t::SHUTDOWN_NOW);
    release.count_down();
    thread_pool_test_access::join(pool);
    assert(first.get() == 1);
    assert(thread_pool_test_access::queued(pool) == 2);
    thread_pool_test_access::discard_queued(pool);
    for (auto* result : {&second, &third}) {
        assert(result->wait_for(0s) == std::future_status::ready);
        bool discarded = false;
        try { result->get(); }
        catch (const std::future_error& error) {
            discarded = error.code() == std::make_error_code(std::future_errc::broken_promise);
        }
        assert(discarded);
    }
    assert(thread_pool_test_access::queued(pool) == 0);
}

void idle_workers_exit() {
    for (auto state : {status_t::SHUTDOWN, status_t::SHUTDOWN_NOW, status_t::TERMINATED}) {
        thread_pool pool(2);
        thread_pool_test_access::set_state(pool, status_t::RUNNING);
        thread_pool_test_access::set_state(pool, state);
        thread_pool_test_access::join(pool);
    }
}
}

int main() {
    execution_and_identity();
    paused_and_fifo();
    out_of_order_completion();
    concurrent_producers_execute_once();
    destruction_outside_lock();
    discarded_destruction_outside_lock();
    graceful_shutdown_from_pause();
    immediate_shutdown_preserves_claimed_task();
    idle_workers_exit();
}
