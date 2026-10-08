// 行为断言在所有构建配置中启用；关闭走公共接口，友元只观察状态。
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "thread_pool.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <latch>
#include <memory>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

struct thread_pool_test_access {
    static void check_state(thread_pool& pool, status_t expected, std::thread::id owner = {}) {
        std::lock_guard lock(pool.queue_mutex_);
        assert(pool.state_ == expected);
        assert(pool.closing_thread_id_ == owner);
    }
    static void wait_for_closing(thread_pool& pool, status_t expected, std::thread::id owner) {
        std::unique_lock lock(pool.queue_mutex_);
        assert(pool.task_ready_.wait_for(lock, 5s, [&] {
            return pool.state_ != status_t::RUNNING;
        }));
        assert(pool.state_ == expected);
        assert(pool.closing_thread_id_ == owner);
    }
    static bool is_worker(const thread_pool& pool) { return pool.is_current_worker(); }
    // worker 集合在队列锁外回收；仅在所有关闭调用完成后读它，避免数据竞争。
    static void check_terminated(thread_pool& pool) {
        check_state(pool, status_t::TERMINATED);
        assert(pool.worker_threads_.empty());
        std::lock_guard lock(pool.queue_mutex_);
        assert(pool.tasks_.empty());
    }
};

namespace {
void close_pool(thread_pool& pool, status_t mode) {
    if (mode == status_t::SHUTDOWN) { pool.shutdown(); }
    else { pool.shutdown_now(); }
}

struct worker_exit_record {
    std::atomic<int>* exits = nullptr;
    ~worker_exit_record() {
        if (exits) { ++*exits; }
    }
};

struct held_pool {
    std::latch entered;
    std::latch release{1};
    std::atomic<int> completed{0}, exited{0};
    std::vector<std::future<int>> tasks;
    bool released = false;
    const std::size_t worker_count;
    thread_pool pool;

    explicit held_pool(std::size_t count)
        : entered(static_cast<std::ptrdiff_t>(count)), worker_count(count), pool(count) {
        for (std::size_t index = 0; index < count; ++index) {
            tasks.push_back(pool.submit([this, index] {
                thread_local worker_exit_record record;
                record.exits = &exited;
                entered.count_down();
                release.wait();
                ++completed;
                return static_cast<int>(index);
            }));
        }
        entered.wait();
    }
    void unblock() {
        if (!released) {
            released = true;
            release.count_down();
        }
    }
    ~held_pool() { unblock(); }
    void check_finished() {
        for (std::size_t index = 0; index < worker_count; ++index) {
            assert(tasks[index].wait_for(0s) == std::future_status::ready);
            assert(tasks[index].get() == static_cast<int>(index));
        }
        assert(completed.load() == static_cast<int>(worker_count));
        assert(exited.load() == static_cast<int>(worker_count));
        thread_pool_test_access::check_terminated(pool);
    }
};

void check_done(std::future<void>& result) {
    assert(result.wait_for(5s) == std::future_status::ready);
    result.get();
}

void check_broken(std::future<int>& result) {
    assert(result.wait_for(5s) == std::future_status::ready);
    bool broken = false;
    try { (void)result.get(); }
    catch (const std::future_error& error) {
        broken = error.code() == std::make_error_code(std::future_errc::broken_promise);
    }
    assert(broken);
}

void check_self_rejected(thread_pool& pool) {
    for (const auto mode : {status_t::SHUTDOWN, status_t::SHUTDOWN_NOW}) {
        bool rejected = false;
        try { close_pool(pool, mode); }
        catch (const std::logic_error&) { rejected = true; }
        assert(rejected);
    }
}

struct mixed_callers {
    static constexpr int count = 8;
    std::latch ready{count}, start{1}, calling{count};
    std::array<std::future<void>, count> results;

    explicit mixed_callers(thread_pool& pool) {
        for (int index = 0; index < count; ++index) {
            results[index] = std::async(std::launch::async, [this, &pool, index] {
                ready.count_down();
                start.wait();
                calling.count_down();
                close_pool(pool, index % 2 == 0 ? status_t::SHUTDOWN : status_t::SHUTDOWN_NOW);
            });
        }
        ready.wait();
        start.count_down();
        calling.wait();
    }
    void check_waiting() {
        for (auto& result : results) {
            assert(result.wait_for(0s) == std::future_status::timeout);
        }
    }
    void finish() {
        for (auto& result : results) { check_done(result); }
    }
};

void first_mode(std::size_t count) {
    constexpr int task_count = 100;
    for (const auto mode : {status_t::SHUTDOWN, status_t::SHUTDOWN_NOW}) {
        std::array<std::atomic<int>, task_count> calls{};
        held_pool fixture(count);
        std::vector<std::future<int>> queued;
        for (int id = 0; id < task_count; ++id) {
            queued.push_back(fixture.pool.submit([&, id] { ++calls[id]; return id; }));
        }
        std::promise<std::thread::id> identity;
        auto owner_id = identity.get_future();
        auto owner = std::async(std::launch::async, [&] {
            identity.set_value(std::this_thread::get_id());
            close_pool(fixture.pool, mode);
        });
        const auto id = owner_id.get();
        thread_pool_test_access::wait_for_closing(fixture.pool, mode, id);
        mixed_callers waiters(fixture.pool);
        waiters.check_waiting();
        assert(owner.wait_for(0s) == std::future_status::timeout);
        thread_pool_test_access::check_state(fixture.pool, mode, id);
        if (mode == status_t::SHUTDOWN_NOW) {
            for (auto& result : queued) { check_broken(result); }
        } else {
            for (auto& result : queued) {
                assert(result.wait_for(0s) == std::future_status::timeout);
            }
        }
        fixture.unblock();
        check_done(owner);
        waiters.finish();
        fixture.check_finished();
        for (int task = 0; task < task_count; ++task) {
            if (mode == status_t::SHUTDOWN) { assert(queued[task].get() == task); }
            assert(calls[task].load() == (mode == status_t::SHUTDOWN ? 1 : 0));
        }
    }
}

struct discard_barrier {
    std::latch& entered;
    std::latch& release;
    std::latch& finished;
    ~discard_barrier() {
        entered.count_down();
        release.wait();
        finished.count_down();
    }
};

void waiters_wait(std::size_t count) {
    std::latch discarding(1), release_discard(1), discarded(1);
    held_pool fixture(count);
    auto probe = std::unique_ptr<discard_barrier>(new discard_barrier{
        discarding, release_discard, discarded});
    // future 立即释放，使丢弃 Task 必然在关闭负责人线程上销毁捕获对象。
    (void)fixture.pool.submit([owned = std::move(probe)] { assert(false); });
    std::promise<std::thread::id> identity;
    auto owner_id = identity.get_future();
    auto owner = std::async(std::launch::async, [&] {
        identity.set_value(std::this_thread::get_id());
        fixture.pool.shutdown_now();
    });
    const auto id = owner_id.get();
    discarding.wait();
    thread_pool_test_access::check_state(fixture.pool, status_t::SHUTDOWN_NOW, id);
    mixed_callers waiters(fixture.pool);
    waiters.check_waiting(); // 第一个阶段：负责人正阻塞在用户对象析构中。
    assert(owner.wait_for(0s) == std::future_status::timeout);
    release_discard.count_down();
    discarded.wait();
    waiters.check_waiting(); // 第二个阶段：析构已放行，但 worker 仍在运行。
    assert(owner.wait_for(0s) == std::future_status::timeout);
    thread_pool_test_access::check_state(fixture.pool, status_t::SHUTDOWN_NOW, id);
    fixture.unblock();
    check_done(owner);
    waiters.finish();
    fixture.check_finished();
}

void worker_self_close(std::size_t count) {
    // 同时覆盖 RUNNING 和已有外部关闭负责人时的自等待拒绝。
    for (const auto mode : {status_t::RUNNING, status_t::SHUTDOWN, status_t::SHUTDOWN_NOW}) {
        std::latch entered(static_cast<std::ptrdiff_t>(count)), release(1);
        thread_pool pool(count);
        std::vector<std::future<int>> results;
        for (std::size_t index = 0; index < count; ++index) {
            results.push_back(pool.submit([&, index] {
                entered.count_down();
                release.wait();
                assert(thread_pool_test_access::is_worker(pool));
                check_self_rejected(pool);
                return static_cast<int>(index);
            }));
        }
        entered.wait();
        std::promise<std::thread::id> identity;
        auto owner_id = identity.get_future();
        std::future<void> owner;
        if (mode != status_t::RUNNING) {
            owner = std::async(std::launch::async, [&] {
                identity.set_value(std::this_thread::get_id());
                close_pool(pool, mode);
            });
            thread_pool_test_access::wait_for_closing(pool, mode, owner_id.get());
        }
        release.count_down();
        for (std::size_t index = 0; index < count; ++index) {
            assert(results[index].wait_for(5s) == std::future_status::ready);
            assert(results[index].get() == static_cast<int>(index));
        }
        if (owner.valid()) { check_done(owner); }
        else {
            thread_pool_test_access::check_state(pool, status_t::RUNNING);
            assert(pool.submit([] { return 42; }).get() == 42);
            pool.shutdown();
        }
        thread_pool_test_access::check_terminated(pool);
    }
}

struct reentry_report {
    std::thread::id thread;
    bool worker;
    int logic_errors = 0;
};
struct reentry_probe {
    thread_pool& pool;
    std::promise<reentry_report>& report;
    ~reentry_probe() {
        reentry_report result{std::this_thread::get_id(), thread_pool_test_access::is_worker(pool)};
        for (const auto mode : {status_t::SHUTDOWN, status_t::SHUTDOWN_NOW}) {
            try { close_pool(pool, mode); }
            catch (const std::logic_error&) { ++result.logic_errors; }
        }
        report.set_value(result);
    }
};

void owner_reentry(std::size_t count) {
    std::promise<reentry_report> report;
    auto observed = report.get_future();
    held_pool fixture(count);
    auto probe = std::unique_ptr<reentry_probe>(new reentry_probe{fixture.pool, report});
    (void)fixture.pool.submit([owned = std::move(probe)] { assert(false); });
    std::promise<std::thread::id> identity;
    auto owner_id = identity.get_future();
    auto owner = std::async(std::launch::async, [&] {
        identity.set_value(std::this_thread::get_id());
        fixture.pool.shutdown_now();
    });
    const auto id = owner_id.get();
    assert(observed.wait_for(5s) == std::future_status::ready);
    const auto result = observed.get();
    assert(result.thread == id && !result.worker && result.logic_errors == 2);
    thread_pool_test_access::check_state(fixture.pool, status_t::SHUTDOWN_NOW, id);
    assert(owner.wait_for(0s) == std::future_status::timeout);
    fixture.unblock();
    check_done(owner);
    fixture.check_finished();
}

void other_pool_worker(std::size_t count) {
    for (const auto mode : {status_t::SHUTDOWN, status_t::SHUTDOWN_NOW}) {
        held_pool target(count);
        thread_pool source(count);
        auto queued = target.pool.submit([] { return 17; });
        std::promise<std::thread::id> identity;
        auto owner_id = identity.get_future();
        auto closing = source.submit([&] {
            assert(thread_pool_test_access::is_worker(source));
            assert(!thread_pool_test_access::is_worker(target.pool));
            identity.set_value(std::this_thread::get_id());
            close_pool(target.pool, mode); // 其他池的 worker 可以成为本池关闭负责人。
            assert(thread_pool_test_access::is_worker(source));
            assert(!thread_pool_test_access::is_worker(target.pool));
            check_self_rejected(source); // 关闭其他池不会破坏本池 worker 的身份。
        });
        thread_pool_test_access::wait_for_closing(target.pool, mode, owner_id.get());
        if (mode == status_t::SHUTDOWN_NOW) { check_broken(queued); }
        else { assert(queued.wait_for(0s) == std::future_status::timeout); }
        assert(closing.wait_for(0s) == std::future_status::timeout);
        target.unblock();
        check_done(closing);
        target.check_finished();
        if (mode == status_t::SHUTDOWN) { assert(queued.get() == 17); }
        thread_pool_test_access::check_state(source, status_t::RUNNING);
        assert(source.submit([] { return 42; }).get() == 42);
        source.shutdown();
    }
}

void repeated_close(std::size_t count) {
    for (const auto mode : {status_t::SHUTDOWN, status_t::SHUTDOWN_NOW}) {
        held_pool fixture(count);
        fixture.unblock();
        close_pool(fixture.pool, mode);
        fixture.check_finished();
        auto repeating = std::async(std::launch::async, [&] {
            for (int repeat = 0; repeat < 100; ++repeat) {
                fixture.pool.shutdown();
                fixture.pool.shutdown_now();
            }
        });
        check_done(repeating);
        mixed_callers waiters(fixture.pool);
        waiters.finish(); // TERMINATED 后并发混用两入口也正常返回。
        thread_pool_test_access::check_terminated(fixture.pool);
        assert(fixture.exited.load() == static_cast<int>(count));
        bool rejected = false;
        try { (void)fixture.pool.submit([] {}); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
}

struct test_case {
    std::string_view name;
    void (*run)(std::size_t);
};
constexpr std::array cases{
    test_case{"first_mode", first_mode},
    test_case{"waiters_wait", waiters_wait},
    test_case{"worker_self_close", worker_self_close},
    test_case{"owner_reentry", owner_reentry},
    test_case{"other_pool_worker", other_pool_worker},
    test_case{"repeated_close", repeated_close}
};
}

int main(int argc, char* argv[]) {
    if (argc == 2) {
        for (const auto& test : cases) {
            if (test.name == argv[1]) {
                for (const std::size_t count : {1U, 4U}) { test.run(count); }
                return 0;
            }
        }
    }
    std::cerr << "Expected one shutdown race and self-wait test case name\n";
    return 1;
}
