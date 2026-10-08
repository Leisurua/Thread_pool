// 行为断言在所有构建配置中启用；本组仅使用线程池公共接口。
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
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct worker_exit_record {
    std::atomic<int>* exits = nullptr;
    ~worker_exit_record() {
        if (exits) { exits->fetch_add(1, std::memory_order_relaxed); }
    }
};

struct blocked_pool {
    std::latch entered;
    std::latch release{1};
    std::atomic<int> completed{0};
    std::atomic<int> exited{0};
    std::vector<std::future<void>> tasks;
    bool released = false;
    thread_pool pool; // 最先析构，确保测试同步对象和计数活到 worker 退出之后。

    explicit blocked_pool(std::size_t count)
        : entered(static_cast<std::ptrdiff_t>(count)), pool(count) {
        for (std::size_t index = 0; index < count; ++index) {
            tasks.push_back(pool.submit([this] {
                thread_local worker_exit_record record;
                record.exits = &exited;
                entered.count_down();
                release.wait();
                completed.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        entered.wait(); // 全部 worker 已领取任务并阻塞，后续提交只能排队。
    }

    void unblock() {
        if (!released) {
            released = true;
            release.count_down();
        }
    }
    ~blocked_pool() { unblock(); }

    void check_finished(std::size_t count) {
        for (auto& task : tasks) {
            assert(task.wait_for(0s) == std::future_status::ready);
            task.get();
        }
        assert(completed.load() == static_cast<int>(count));
        // TLS 析构发生在线程退出时；任务 future 就绪不能代替 join 边界。
        assert(exited.load() == static_cast<int>(count));
    }
};

void check_ready(std::vector<std::future<void>>& results) {
    for (auto& result : results) {
        assert(result.wait_for(0s) == std::future_status::ready);
        result.get();
    }
}

// 公共接口没有状态查询；首次 runtime_error 拒绝是关闭已生效的可观察证据。
// 保存竞态中成功接受的探测任务，关闭返回后同样验证其全部完成。
std::vector<std::future<void>> wait_for_shutdown(thread_pool& pool) {
    std::vector<std::future<void>> accepted;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        try { accepted.push_back(pool.submit([] {})); }
        catch (const std::runtime_error&) { return accepted; }
        std::this_thread::yield();
    }
    assert(false && "shutdown did not reject submission within 5 seconds");
    std::abort();
}

void check_rejected(thread_pool& pool, std::atomic<int>& calls) {
    bool rejected = false;
    try { (void)pool.submit([&] { ++calls; }); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}

void empty_queue(std::size_t count) {
    thread_pool pool(count);
    pool.shutdown(); // 从未提交过任务，空闲 worker 必须全部退出。
}

void drains_queue(std::size_t count) {
    constexpr int task_count = 1000;
    std::array<std::atomic<int>, task_count> calls{};
    blocked_pool fixture(count);
    std::vector<std::future<int>> queued;
    for (int id = 0; id < task_count; ++id) {
        queued.push_back(fixture.pool.submit([&, id] { ++calls[id]; return id; }));
    }
    auto closing = std::async(std::launch::async, [&] { fixture.pool.shutdown(); });
    auto probes = wait_for_shutdown(fixture.pool);
    assert(closing.wait_for(0s) == std::future_status::timeout);
    for (auto& task : queued) {
        assert(task.wait_for(0s) == std::future_status::timeout);
    }
    fixture.unblock();
    closing.get();
    fixture.check_finished(count);
    for (int id = 0; id < task_count; ++id) {
        assert(queued[id].wait_for(0s) == std::future_status::ready);
        assert(queued[id].get() == id);
        assert(calls[id].load() == 1);
    }
    check_ready(probes);
}

void waits_running(std::size_t count) {
    blocked_pool fixture(count);
    auto closing = std::async(std::launch::async, [&] { fixture.pool.shutdown(); });
    auto probes = wait_for_shutdown(fixture.pool);
    assert(closing.wait_for(0s) == std::future_status::timeout);
    assert(fixture.completed.load() == 0 && fixture.exited.load() == 0);
    fixture.unblock();
    closing.get();
    fixture.check_finished(count);
    check_ready(probes);
}

void rejects_submit(std::size_t count) {
    std::atomic<int> calls{0};
    blocked_pool fixture(count);
    auto closing = std::async(std::launch::async, [&] { fixture.pool.shutdown(); });
    auto probes = wait_for_shutdown(fixture.pool);
    assert(closing.wait_for(0s) == std::future_status::timeout);
    for (int attempt = 0; attempt < 32; ++attempt) {
        check_rejected(fixture.pool, calls); // 正在关闭、worker 尚未完成时已拒绝。
    }
    fixture.unblock();
    closing.get();
    fixture.check_finished(count);
    check_ready(probes);
    check_rejected(fixture.pool, calls); // 关闭完成后继续拒绝。
    assert(calls.load() == 0);
}

void repeated_shutdown(std::size_t count) {
    std::atomic<int> calls{0};
    blocked_pool fixture(count);
    fixture.unblock();
    fixture.pool.shutdown();
    fixture.check_finished(count);
    for (int attempt = 0; attempt < 100; ++attempt) {
        fixture.pool.shutdown();
        assert(fixture.exited.load() == static_cast<int>(count));
        assert(fixture.completed.load() == static_cast<int>(count));
    }
    check_rejected(fixture.pool, calls);
    assert(calls.load() == 0);
    // 作用域结束还会运行产品析构，覆盖显式关闭后的再次回收入口。
}

void concurrent_shutdown(std::size_t count) {
    constexpr int caller_count = 8;
    constexpr int queued_count = 100;
    std::array<std::atomic<int>, queued_count> calls{};
    blocked_pool fixture(count);
    std::vector<std::future<int>> queued;
    for (int id = 0; id < queued_count; ++id) {
        queued.push_back(fixture.pool.submit([&, id] { ++calls[id]; return id; }));
    }
    std::latch ready(caller_count), start(1), calling(caller_count);
    std::array<std::future<void>, caller_count> results;
    for (auto& result : results) {
        result = std::async(std::launch::async, [&] {
            ready.count_down();
            start.wait();
            calling.count_down();
            fixture.pool.shutdown();
        });
    }
    ready.wait();
    start.count_down();
    calling.wait();
    auto probes = wait_for_shutdown(fixture.pool);
    for (auto& result : results) {
        assert(result.wait_for(0s) == std::future_status::timeout);
    }
    fixture.unblock();
    for (auto& result : results) { result.get(); } // 全部调用者正常返回，无异常。
    fixture.check_finished(count);
    for (int id = 0; id < queued_count; ++id) {
        assert(queued[id].wait_for(0s) == std::future_status::ready);
        assert(queued[id].get() == id);
        assert(calls[id].load() == 1);
    }
    check_ready(probes);
    fixture.pool.shutdown();
    assert(fixture.exited.load() == static_cast<int>(count));
}

struct test_case {
    std::string_view name;
    void (*run)(std::size_t);
};
constexpr std::array cases{
    test_case{"empty_queue", empty_queue},
    test_case{"drains_queue", drains_queue},
    test_case{"waits_running", waits_running},
    test_case{"rejects_submit", rejects_submit},
    test_case{"repeated_shutdown", repeated_shutdown},
    test_case{"concurrent_shutdown", concurrent_shutdown}
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
    std::cerr << "Expected one graceful shutdown test case name\n";
    return 1;
}
