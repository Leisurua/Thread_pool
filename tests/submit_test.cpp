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
// 只在测试队列扩容时注入分配失败，准备模型和登记 set 节点不受影响。
thread_local std::set<std::uint64_t>* watched_progress = nullptr;
thread_local std::size_t progress_before_push = 0;
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
}

void* operator new(std::size_t size) {
    if (fail_queue_allocation && watched_progress->size() > progress_before_push) {
        fail_queue_allocation = false;
        throw std::bad_alloc{};
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

// 测试专用池生命周期桩：不创建真实 worker，提交测试由友元访问器同步驱动。
thread_pool::thread_pool(std::size_t)
    : state_(status_t::RUNNING), last_accepted_id_(0), first_cancelled_id_(0),
      wait_active_(false), join_in_progress_(false) {}
thread_pool::~thread_pool() = default;

struct thread_pool_test_access {
    static void exercise_submit(thread_pool& pool);
};

void thread_pool_test_access::exercise_submit(thread_pool& pool) {
    auto take = [&] {
        std::lock_guard lock(pool.mutex_);
        assert(!pool.tasks_.empty());
        auto task = std::move(pool.tasks_.front());
        pool.tasks_.pop();
        return task;
    };
    auto finish = [&](std::uint64_t id) {
        pool.finish_task(id, false);
    };
    auto execute_one = [&] {
        auto task = take();
        task(); // 必须在锁外执行，用户异常通过 future 反馈。
        finish(task.id());
    };
    auto drain = [&] {
        while (!pool.tasks_.empty()) {
            execute_one();
        }
        assert(pool.unfinished_tasks_.empty());
    };

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
    assert(pool.tasks_.size() == 5 && pool.unfinished_tasks_.size() == 5);
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
        std::uint64_t id;
        {
            auto task = take();
            id = task.id();
        } // 锁外释放未执行 packaged_task，结果提供方放弃共享状态。
        pool.finish_task(id, true);
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

    // PAUSED 接受提交；三种关闭状态均拒绝，且不推进接受边界。
    pool.state_ = status_t::PAUSED;
    auto paused = pool.submit([] { return 9; });
    assert(paused.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    pool.state_ = status_t::RUNNING;
    drain();
    assert(paused.get() == 9);
    for (auto state : {status_t::SHUTDOWN, status_t::SHUTDOWN_NOW, status_t::TERMINATED}) {
        pool.state_ = state;
        const auto previous_id = pool.last_accepted_id_;
        bool rejected = false;
        try { (void)pool.submit([] { return 1; }); }
        catch (const std::runtime_error& error) {
            rejected = std::string(error.what()) == "thread pool submission rejected";
        }
        assert(rejected && pool.last_accepted_id_ == previous_id);
        assert(pool.tasks_.empty() && pool.unfinished_tasks_.empty());
    }
    pool.state_ = status_t::RUNNING;

    // 用户复制构造抛出的 runtime_error 派生异常保持类型与信息。
    copy_failure callable;
    const auto id_before_copy = pool.last_accepted_id_;
    original_error = false;
    try { (void)pool.submit(callable); }
    catch (const user_error& error) {
        original_error = std::string(error.what()) == "copy failure";
    }
    assert(original_error && pool.last_accepted_id_ == id_before_copy);
    assert(pool.tasks_.empty() && pool.unfinished_tasks_.empty());

    // 准备工作不占主锁，接受编号按入队顺序分配，而不是按 submit 开始顺序。
    std::latch entered(1), release(1);
    slow_copy slow(entered, release);
    std::future<int> slow_result;
    const auto id_before_race = pool.last_accepted_id_;
    std::jthread submitting([&] { slow_result = pool.submit(slow); });
    entered.wait();
    auto fast_result = pool.submit([] { return 1; });
    assert(pool.tasks_.front().id() == id_before_race + 1);
    execute_one();
    assert(fast_result.get() == 1);
    release.count_down();
    submitting.join();
    assert(pool.tasks_.front().id() == id_before_race + 2);
    drain();
    assert(slow_result.get() == 2);

    // 多线程提交；所有任务只入队一次，编号与 unfinished set 保持一致。
    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = 24;
    std::vector<std::future<int>> results[producer_count];
    std::vector<std::jthread> producers;
    const auto id_before_producers = pool.last_accepted_id_;
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = 0; index < tasks_per_producer; ++index) {
                const int expected = producer * tasks_per_producer + index;
                results[producer].push_back(pool.submit([expected] { return expected; }));
            }
        });
    }
    producers.clear(); // jthread 析构等待提交线程完成。
    assert(pool.tasks_.size() == producer_count * tasks_per_producer);
    auto expected_id = id_before_producers + 1;
    while (!pool.tasks_.empty()) {
        assert(pool.tasks_.front().id() == expected_id++);
        assert(*pool.unfinished_tasks_.begin() == pool.tasks_.front().id());
        execute_one();
    }
    for (int producer = 0; producer < producer_count; ++producer) {
        for (int index = 0; index < tasks_per_producer; ++index) {
            assert(results[producer][index].get() == producer * tasks_per_producer + index);
        }
    }

    // 对 deque 扩容注入失败，验证 push 失败后登记记录和接受边界都回滚。
    std::vector<std::future<int>> before_allocation_failure;
    before_allocation_failure.reserve(2048);
    watched_progress = &pool.unfinished_tasks_;
    bool allocation_failed = false;
    for (int attempt = 0; attempt < 2048 && !allocation_failed; ++attempt) {
        const auto previous_id = pool.last_accepted_id_;
        const auto previous_size = pool.tasks_.size();
        progress_before_push = pool.unfinished_tasks_.size();
        fail_queue_allocation = true;
        try {
            before_allocation_failure.push_back(pool.submit([] { return 3; }));
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
            assert(pool.last_accepted_id_ == previous_id);
            assert(pool.tasks_.size() == previous_size);
            assert(pool.unfinished_tasks_.size() == progress_before_push);
        }
        fail_queue_allocation = false;
    }
    watched_progress = nullptr;
    assert(allocation_failed);
    auto retry = pool.submit([] { return 4; });
    drain();
    for (auto& result : before_allocation_failure) { assert(result.get() == 3); }
    assert(retry.get() == 4);

    const auto saved_id = pool.last_accepted_id_;
    pool.last_accepted_id_ = std::numeric_limits<std::uint64_t>::max();
    bool overflow_reported = false;
    try { (void)pool.submit([] { return 1; }); }
    catch (const std::overflow_error& error) {
        overflow_reported = std::string(error.what()) == "thread pool task ID exhausted";
    }
    assert(overflow_reported && pool.tasks_.empty() && pool.unfinished_tasks_.empty());
    assert(pool.last_accepted_id_ == std::numeric_limits<std::uint64_t>::max());
    pool.last_accepted_id_ = saved_id;
}

int main() {
    thread_pool pool(1);
    thread_pool_test_access::exercise_submit(pool);
}
