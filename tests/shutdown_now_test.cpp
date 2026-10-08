// 行为断言在所有构建配置中启用；本组仅使用线程池公共接口。
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "thread_pool.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct exit_observer {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t count = 0;

    void record() {
        {
            std::lock_guard lock(mutex);
            ++count;
        }
        changed.notify_all();
    }
    bool wait_for(std::size_t expected) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return count >= expected; });
    }
    std::size_t observed() {
        std::lock_guard lock(mutex);
        return count;
    }
};

struct worker_exit_record {
    exit_observer* observer = nullptr;
    ~worker_exit_record() {
        if (observer) { observer->record(); }
    }
};

struct blocked_pool {
    std::latch entered;
    std::array<std::latch, 4> release{
        std::latch{1}, std::latch{1}, std::latch{1}, std::latch{1}};
    std::array<bool, 4> released{};
    std::array<std::atomic<int>, 4> completed{};
    exit_observer exits;
    std::vector<std::future<int>> tasks;
    const std::size_t worker_count;
    thread_pool pool; // 先析构池，保证同步对象和退出观察器活到 worker join 之后。

    explicit blocked_pool(std::size_t count)
        : entered(static_cast<std::ptrdiff_t>(count)), worker_count(count), pool(count) {
        assert(count <= release.size());
        for (std::size_t index = 0; index < count; ++index) {
            tasks.push_back(pool.submit([this, index] {
                thread_local worker_exit_record record;
                record.observer = &exits;
                entered.count_down();
                release[index].wait();
                ++completed[index];
                return static_cast<int>(100 + index);
            }));
        }
        entered.wait(); // 每个 worker 均已领取一个任务，后续任务确定只能排队。
    }
    void unblock(std::size_t index) {
        if (!released[index]) {
            released[index] = true;
            release[index].count_down();
        }
    }
    void unblock_all() {
        for (std::size_t index = 0; index < worker_count; ++index) { unblock(index); }
    }
    ~blocked_pool() { unblock_all(); }

    void check_finished() {
        for (std::size_t index = 0; index < worker_count; ++index) {
            if (tasks[index].valid()) {
                assert(tasks[index].wait_for(0s) == std::future_status::ready);
                assert(tasks[index].get() == static_cast<int>(100 + index));
            }
            assert(completed[index].load() == 1);
        }
        assert(exits.observed() == worker_count);
    }
};

template<class R>
void check_broken_promise(std::future<R>& result) {
    assert(result.wait_for(5s) == std::future_status::ready);
    bool broken = false;
    try { (void)result.get(); }
    catch (const std::future_error& error) {
        broken = error.code() == std::make_error_code(std::future_errc::broken_promise);
    }
    assert(broken);
}

void finish_close(std::future<void>& closing, blocked_pool& fixture) {
    assert(closing.wait_for(5s) == std::future_status::ready);
    closing.get();
    fixture.check_finished();
}

void discards_unclaimed(std::size_t count) {
    constexpr int task_count = 1000;
    std::array<std::atomic<int>, task_count> calls{};
    blocked_pool fixture(count);
    std::vector<std::future<int>> queued;
    for (int id = 0; id < task_count; ++id) {
        queued.push_back(fixture.pool.submit([&, id] { ++calls[id]; return id; }));
    }
    auto closing = std::async(std::launch::async, [&] { fixture.pool.shutdown_now(); });
    for (auto& result : queued) { check_broken_promise(result); }
    for (const auto& calls_for_task : calls) { assert(calls_for_task.load() == 0); }
    assert(closing.wait_for(0s) == std::future_status::timeout);
    fixture.unblock_all();
    finish_close(closing, fixture);
    // join 后再次逐 ID 核对，不能有解除阻塞后才执行的被丢弃任务。
    for (const auto& calls_for_task : calls) { assert(calls_for_task.load() == 0); }
}

void discarded_future(std::size_t count) {
    std::atomic<int> calls{0};
    int target = 42;
    blocked_pool fixture(count);
    auto value = fixture.pool.submit([&] { ++calls; return 7; });
    auto nothing = fixture.pool.submit([&] { ++calls; });
    auto reference = fixture.pool.submit([&]() -> int& { ++calls; return target; });
    auto movable = fixture.pool.submit([&] { ++calls; return std::make_unique<int>(9); });
    auto closing = std::async(std::launch::async, [&] { fixture.pool.shutdown_now(); });
    // 关闭调用仍在等待已领取任务时，丢弃结果就已反馈，非等待池析构才反馈。
    check_broken_promise(value);
    check_broken_promise(nothing);
    check_broken_promise(reference);
    check_broken_promise(movable);
    assert(calls.load() == 0);
    assert(closing.wait_for(0s) == std::future_status::timeout);
    fixture.unblock_all();
    finish_close(closing, fixture);
    assert(calls.load() == 0 && target == 42);
}

void claimed_result(std::size_t count) {
    blocked_pool fixture(count);
    auto discarded = fixture.pool.submit([] { return -1; });
    auto closing = std::async(std::launch::async, [&] { fixture.pool.shutdown_now(); });
    check_broken_promise(discarded); // 证明立即关闭已生效，再恢复已领取的任务。
    for (auto& task : fixture.tasks) {
        assert(task.wait_for(0s) == std::future_status::timeout);
    }
    fixture.unblock_all();
    finish_close(closing, fixture); // 所有已领取任务正常返回原结果，且各执行一次。
}

void idle_workers_exit(std::size_t count) {
    {
        thread_pool pool(count); // 从未提交任务的空闲池。
        auto closing = std::async(std::launch::async, [&] { pool.shutdown_now(); });
        assert(closing.wait_for(5s) == std::future_status::ready);
        closing.get();
    }
    {
        blocked_pool fixture(count);
        fixture.unblock_all();
        for (std::size_t index = 0; index < count; ++index) {
            assert(fixture.tasks[index].get() == static_cast<int>(100 + index));
        }
        auto closing = std::async(std::launch::async, [&] { fixture.pool.shutdown_now(); });
        finish_close(closing, fixture); // 任务完成后队列为空，TLS 记录确认全部退出。
    }
    if (count > 1) {
        blocked_pool fixture(count);
        for (std::size_t index = 0; index + 1 < count; ++index) {
            fixture.unblock(index);
            assert(fixture.tasks[index].get() == static_cast<int>(100 + index));
        }
        auto closing = std::async(std::launch::async, [&] { fixture.pool.shutdown_now(); });
        // 保留一个运行任务；其他空闲 worker 仍须独立退出，不等它完成。
        assert(fixture.exits.wait_for(count - 1));
        assert(fixture.exits.observed() == count - 1);
        assert(fixture.tasks.back().wait_for(0s) == std::future_status::timeout);
        assert(closing.wait_for(0s) == std::future_status::timeout);
        fixture.unblock(count - 1);
        finish_close(closing, fixture);
    }
}

void claim_race(std::size_t count) {
    constexpr int rounds = 100;
    constexpr int task_count = 256;
    int total_executed = 0, total_discarded = 0;
    for (int round = 0; round < rounds; ++round) {
        std::array<std::atomic<int>, task_count> calls{};
        blocked_pool fixture(count);
        std::vector<std::future<int>> results;
        for (int id = 0; id < task_count; ++id) {
            results.push_back(fixture.pool.submit([&, id] { ++calls[id]; return id; }));
        }
        std::latch ready(2), start(1);
        auto releasing = std::async(std::launch::async, [&] {
            ready.count_down();
            start.wait();
            fixture.unblock_all(); // 放行 worker，使下一轮领取与关闭竞争队列锁。
        });
        auto closing = std::async(std::launch::async, [&] {
            ready.count_down();
            start.wait();
            fixture.pool.shutdown_now();
        });
        ready.wait();
        start.count_down();
        releasing.get();
        finish_close(closing, fixture);
        int executed = 0, discarded = 0;
        for (int id = 0; id < task_count; ++id) {
            assert(results[id].wait_for(0s) == std::future_status::ready);
            try {
                assert(results[id].get() == id);
                assert(calls[id].load() == 1);
                ++executed;
            } catch (const std::future_error& error) {
                assert(error.code() == std::make_error_code(std::future_errc::broken_promise));
                assert(calls[id].load() == 0);
                ++discarded;
            }
        }
        assert(executed + discarded == task_count);
        total_executed += executed;
        total_discarded += discarded;
    }
    std::cout << "claim_race: workers=" << count << ", rounds=" << rounds
              << ", executed=" << total_executed << ", discarded=" << total_discarded << '\n';
}

void waits_running(std::size_t count) {
    blocked_pool fixture(count);
    auto discarded = fixture.pool.submit([] {});
    auto closing = std::async(std::launch::async, [&] { fixture.pool.shutdown_now(); });
    check_broken_promise(discarded);
    assert(closing.wait_for(0s) == std::future_status::timeout);
    // 逐个恢复任务；只要还剩一个运行任务，shutdown_now 就不得返回。
    for (std::size_t index = 0; index < count; ++index) {
        fixture.unblock(index);
        assert(fixture.tasks[index].get() == static_cast<int>(100 + index));
        if (index + 1 < count) {
            assert(fixture.tasks.back().wait_for(0s) == std::future_status::timeout);
            assert(closing.wait_for(0s) == std::future_status::timeout);
        }
    }
    finish_close(closing, fixture);
}

struct test_case {
    std::string_view name;
    void (*run)(std::size_t);
};
constexpr std::array cases{
    test_case{"discards_unclaimed", discards_unclaimed},
    test_case{"discarded_future", discarded_future},
    test_case{"claimed_result", claimed_result},
    test_case{"idle_workers_exit", idle_workers_exit},
    test_case{"claim_race", claim_race},
    test_case{"waits_running", waits_running}
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
    std::cerr << "Expected one immediate shutdown test case name\n";
    return 1;
}
