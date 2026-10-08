// 行为断言在所有构建配置中启用；本组仅使用线程池公共接口。
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "thread_pool.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <functional>
#include <iostream>
#include <latch>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
void workers(std::size_t worker_count) {
    std::latch entered(static_cast<std::ptrdiff_t>(worker_count)), release(1);
    thread_pool pool(worker_count);
    std::vector<std::future<std::thread::id>> results;
    for (std::size_t index = 0; index < worker_count; ++index) {
        results.push_back(pool.submit([&] {
            entered.count_down();
            release.wait();
            return std::this_thread::get_id();
        }));
    }
    // 每个任务占住一个 worker，确定多个 worker 确实同时执行任务。
    entered.wait();
    release.count_down();
    std::set<std::thread::id> identities;
    for (auto& result : results) {
        const auto identity = result.get();
        assert(identity != std::this_thread::get_id());
        identities.insert(identity);
    }
    assert(identities.size() == worker_count);
    assert(pool.submit([] { return 42; }).get() == 42);
}

void exactly_once(std::size_t worker_count) {
    constexpr int task_count = 1000;
    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = task_count / producer_count;
    std::array<std::atomic<int>, task_count> calls{};
    std::array<std::vector<std::future<int>>, producer_count> results;
    std::latch start(producer_count);
    thread_pool pool(worker_count);
    std::vector<std::jthread> producers;
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            results[producer].reserve(tasks_per_producer);
            start.count_down();
            start.wait();
            for (int index = 0; index < tasks_per_producer; ++index) {
                const int id = producer * tasks_per_producer + index;
                results[producer].push_back(pool.submit([&, id] {
                    calls[id].fetch_add(1, std::memory_order_relaxed);
                    return id;
                }));
            }
        });
    }
    producers.clear(); // join 所有生产者后再读取各自的 future 容器。
    for (int producer = 0; producer < producer_count; ++producer) {
        assert(results[producer].size() == tasks_per_producer);
        for (int index = 0; index < tasks_per_producer; ++index) {
            assert(results[producer][index].get() == producer * tasks_per_producer + index);
        }
    }
    pool.shutdown(); // worker 全部退出后逐 ID 核对，包含任何迟到的重复执行。
    for (const auto& count : calls) {
        assert(count.load(std::memory_order_relaxed) == 1);
    }
}

void future_results(std::size_t worker_count) {
    int target = 20;
    thread_pool pool(worker_count);
    auto value = pool.submit([](int input) { return input * 2; }, 21);
    auto nothing = pool.submit([&] { ++target; });
    static_assert(std::is_same_v<decltype(value), std::future<int>>);
    static_assert(std::is_same_v<decltype(nothing), std::future<void>>);
    assert(value.get() == 42);
    nothing.get(); // 同步写入后再提交引用任务，多 worker 不依赖完成顺序。
    assert(target == 21);
    auto reference = pool.submit([&]() -> int& { return target; });
    static_assert(std::is_same_v<decltype(reference), std::future<int&>>);
    int& returned = reference.get();
    assert(&returned == &target);
    returned = 37;
    assert(target == 37);
}

struct move_only_callable {
    std::unique_ptr<int> offset;

    int operator()(std::unique_ptr<int> input) & { return (*offset)++ + *input; }
    int operator()(std::unique_ptr<int>) && = delete;
};
static_assert(!std::is_copy_constructible_v<move_only_callable>);
static_assert(std::is_move_constructible_v<move_only_callable>);

void move_only_and_ref(std::size_t worker_count) {
    auto borrowed = std::make_unique<int>(10);
    move_only_callable callable{std::make_unique<int>(5)};
    thread_pool pool(worker_count);
    auto argument = std::make_unique<int>(4);
    auto moved = pool.submit(move_only_callable{std::make_unique<int>(3)},
                             std::move(argument));
    assert(!argument);
    assert(moved.get() == 7); // move-only callable 左值调用，参数按值接收并消耗。
    auto captured = pool.submit([owned = std::make_unique<int>(11)] { return *owned; });
    assert(captured.get() == 11);
    auto reference_argument = pool.submit([](std::unique_ptr<int>& input) {
        return ++*input;
    }, std::ref(borrowed));
    assert(reference_argument.get() == 11);
    assert(borrowed && *borrowed == 11); // 非复制对象仍由调用方持有，修改作用于原对象。
    auto reference_callable = pool.submit(std::ref(callable), std::make_unique<int>(7));
    assert(reference_callable.get() == 12);
    assert(*callable.offset == 6); // std::ref 同样可借用非复制 callable。
}

struct user_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void exception_recovery(std::size_t worker_count) {
    std::thread::id throwing_worker;
    thread_pool pool(worker_count);
    auto failed = pool.submit([&]() -> int {
        throwing_worker = std::this_thread::get_id();
        throw user_error("task failed");
    });
    bool original_error = false;
    try { (void)failed.get(); }
    catch (const user_error& error) {
        original_error = std::string(error.what()) == "task failed";
    }
    assert(original_error);
    // 确认异常已传播后继续提交，单 worker 时必须由同一个线程继续执行。
    auto survivor = pool.submit([] { return std::this_thread::get_id(); });
    const auto surviving_worker = survivor.get();
    if (worker_count == 1) {
        assert(surviving_worker == throwing_worker);
    }
    auto void_failure = pool.submit([] { throw user_error("void task failed"); });
    original_error = false;
    try { void_failure.get(); }
    catch (const user_error& error) {
        original_error = std::string(error.what()) == "void task failed";
    }
    assert(original_error);
    assert(pool.submit([] { return 17; }).get() == 17);
}

struct test_case {
    std::string_view name;
    void (*run)(std::size_t);
};
constexpr std::array cases{
    test_case{"workers", workers},
    test_case{"exactly_once", exactly_once},
    test_case{"future_results", future_results},
    test_case{"move_only_and_ref", move_only_and_ref},
    test_case{"exception_recovery", exception_recovery}
};
}

int main(int argc, char* argv[]) {
    if (argc == 2) {
        for (const auto& test : cases) {
            if (test.name == argv[1]) {
                for (const std::size_t count : {1U, 4U}) {
                    test.run(count);
                }
                return 0;
            }
        }
    }
    std::cerr << "Expected one basic execution test case name\n";
    return 1;
}
