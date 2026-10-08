#include "thread_pool.hpp"
#include "workloads.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <latch>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;
using work_function = std::uint64_t (*)(std::uint64_t, std::uint64_t);
constexpr std::uint64_t seed_base = 0x123456789abcdef0ULL;
constexpr std::uint64_t close_iterations = 2000000;
constexpr std::uint32_t shuffle_seed = 20261008;

std::uint64_t seed_for(std::size_t index) { return seed_base ^ (index + 1); }
std::uint64_t ns_between(clock_type::time_point first, clock_type::time_point last) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(last - first).count());
}
void require(bool condition, std::string_view message) {
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

void warm_workers(thread_pool& pool, std::size_t count) {
    std::latch entered(static_cast<std::ptrdiff_t>(count)), release(1);
    std::vector<std::future<void>> results;
    results.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        results.push_back(pool.submit([&] { entered.count_down(); release.wait(); }));
    }
    entered.wait();
    release.count_down();
    for (auto& result : results) { result.get(); }
}

struct config {
    std::string suite;
    std::string implementation;
    std::size_t workers;
    std::size_t tasks;
    std::size_t busy;
    std::uint64_t iterations;
    work_function work;
    std::uint64_t expected;
};
struct sample {
    std::uint64_t submit_ns = 0, collect_ns = 0, total_ns = 0, close_ns = 0;
    std::uint64_t checksum = 0;
    std::uint64_t first_feedback_ns = 0, all_feedback_ns = 0, release_to_return_ns = 0;
    std::uint64_t future_cleanup_ns = 0, broken_count = 0, executed_count = 0;
};

std::uint64_t reference(work_function work, std::size_t tasks, std::uint64_t iterations) {
    std::uint64_t sum = 0;
    for (std::size_t index = 0; index < tasks; ++index) {
        sum += work(seed_for(index), iterations);
    }
    return sum; // uint64_t 模加校验，溢出有定义。
}

sample task_sample(const config& cfg) {
    sample result;
    if (cfg.workers == 0) {
        const auto begin = clock_type::now();
        for (std::size_t index = 0; index < cfg.tasks; ++index) {
            result.checksum += cfg.work(seed_for(index), cfg.iterations);
        }
        const auto end = clock_type::now();
        result.total_ns = ns_between(begin, end);
    } else {
        thread_pool pool(cfg.workers);
        warm_workers(pool, cfg.workers);
        std::vector<std::future<std::uint64_t>> results;
        results.reserve(cfg.tasks); // future 容器预分配、建池与 worker 启动均不计时。
        const auto begin = clock_type::now();
        for (std::size_t index = 0; index < cfg.tasks; ++index) {
            results.push_back(pool.submit(cfg.work, seed_for(index), cfg.iterations));
        }
        const auto submitted = clock_type::now();
        for (auto& future : results) { result.checksum += future.get(); }
        const auto completed = clock_type::now();
        result.submit_ns = ns_between(begin, submitted);
        result.collect_ns = ns_between(submitted, completed);
        result.total_ns = ns_between(begin, completed);
        pool.shutdown(); // 回收发生在任务计时区间之外。
    }
    require(result.checksum == cfg.expected, "task checksum mismatch");
    require(result.total_ns > 0, "timer resolution too low");
    return result;
}

sample close_sample(const config& cfg) {
    std::latch entered(static_cast<std::ptrdiff_t>(cfg.busy)), release(1), closing(1);
    std::atomic<std::size_t> active{0};
    thread_pool pool(cfg.workers);
    warm_workers(pool, cfg.workers);
    std::vector<std::future<std::uint64_t>> results;
    results.reserve(cfg.busy);
    for (std::size_t index = 0; index < cfg.busy; ++index) {
        results.push_back(pool.submit([&, index] {
            active.fetch_add(1, std::memory_order_relaxed);
            entered.count_down();
            release.wait();
            const auto value = benchmark_workloads::mix(seed_for(index), cfg.iterations);
            active.fetch_sub(1, std::memory_order_relaxed);
            return value;
        }));
    }
    entered.wait(); // 明确计时前实际繁忙 worker 数量，队列没有未领取任务。
    std::jthread releaser([&] { closing.wait(); release.count_down(); });
    require(active.load(std::memory_order_relaxed) == cfg.busy, "busy worker count mismatch");
    sample result;
    const auto begin = clock_type::now();
    closing.count_down(); // 关闭计时包含共同的放行信号及 worker 剩余固定计算。
    if (cfg.implementation == "shutdown") { pool.shutdown(); }
    else { pool.shutdown_now(); }
    const auto end = clock_type::now();
    result.close_ns = ns_between(begin, end);
    releaser.join();
    for (auto& future : results) { result.checksum += future.get(); }
    require(active.load(std::memory_order_relaxed) == 0, "shutdown returned with active workers");
    require(result.checksum == cfg.expected, "close checksum mismatch");
    return result;
}

sample paused_sample(const config& cfg) {
    std::latch entered(static_cast<std::ptrdiff_t>(cfg.workers)), release(1);
    thread_pool pool(cfg.workers);
    warm_workers(pool, cfg.workers);
    std::vector<std::future<void>> blockers;
    for (std::size_t index = 0; index < cfg.workers; ++index) {
        blockers.push_back(pool.submit([&] { entered.count_down(); release.wait(); }));
    }
    entered.wait(); // 全部 worker 已领取受控任务，不能领取基准任务。
    std::vector<std::future<std::uint64_t>> results;
    results.reserve(cfg.tasks);
    sample result;
    const auto begin = clock_type::now();
    for (std::size_t index = 0; index < cfg.tasks; ++index) {
        results.push_back(pool.submit(cfg.work, seed_for(index), cfg.iterations));
    }
    const auto submitted = clock_type::now();
    release.count_down();
    for (auto& future : results) { result.checksum += future.get(); }
    const auto completed = clock_type::now();
    result.submit_ns = ns_between(begin, submitted);
    result.collect_ns = ns_between(submitted, completed);
    result.total_ns = ns_between(begin, completed);
    for (auto& future : blockers) { future.get(); }
    pool.shutdown();
    require(result.checksum == cfg.expected, "paused checksum mismatch");
    return result;
}

sample backlog_sample(const config& cfg) {
    std::latch entered(static_cast<std::ptrdiff_t>(cfg.workers)), release(1), closing(1);
    std::atomic<std::uint64_t> executed{0};
    thread_pool pool(cfg.workers);
    warm_workers(pool, cfg.workers);
    std::vector<std::future<std::uint64_t>> blockers, results;
    blockers.reserve(cfg.workers);
    results.reserve(cfg.tasks);
    for (std::size_t index = 0; index < cfg.workers; ++index) {
        blockers.push_back(pool.submit([&, index] {
            entered.count_down(); release.wait(); return seed_for(index);
        }));
    }
    entered.wait();
    for (std::size_t index = 0; index < cfg.tasks; ++index) {
        results.push_back(pool.submit([&, index] {
            executed.fetch_add(1, std::memory_order_relaxed);
            return benchmark_workloads::tiny(seed_for(index), 0);
        }));
    }
    clock_type::time_point begin, completed;
    std::jthread closer([&] {
        begin = clock_type::now();
        closing.count_down(); // 同步起点；主线程同时观察 future。
        pool.shutdown_now();
        completed = clock_type::now();
    });
    closing.wait();
    sample result;
    bool ready = results.front().wait_for(std::chrono::seconds(30)) == std::future_status::ready;
    const auto first = clock_type::now();
    for (auto& future : results) {
        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
            ready = false; break;
        }
    }
    const auto feedback = clock_type::now();
    release.count_down(); // 反馈全部就绪后才释放已领取任务，确保不消费积压。
    closer.join();
    result.close_ns = ns_between(begin, completed);
    result.first_feedback_ns = ns_between(begin, first);
    result.all_feedback_ns = ns_between(begin, feedback);
    result.release_to_return_ns = ns_between(feedback, completed);
    require(ready, "discard feedback timeout");
    const auto cleanup_begin = clock_type::now();
    for (auto& future : results) {
        try { (void)future.get(); }
        catch (const std::future_error& error) {
            if (error.code() == std::make_error_code(std::future_errc::broken_promise)) {
                ++result.broken_count;
            } else { throw; }
        }
    }
    const auto cleanup_end = clock_type::now();
    result.future_cleanup_ns = ns_between(cleanup_begin, cleanup_end);
    result.executed_count = executed.load(std::memory_order_relaxed);
    for (auto& future : blockers) { result.checksum += future.get(); }
    require(result.broken_count == cfg.tasks, "missing broken_promise");
    require(result.executed_count == 0, "queued callable executed during discard");
    require(result.checksum == cfg.expected, "claimed task result mismatch");
    return result;
}

void write_sample(std::ofstream& csv, const config& cfg, const sample& value,
                  std::string_view phase, int round, std::size_t order) {
    csv << cfg.suite << ',' << cfg.implementation << ',' << cfg.workers << ','
        << cfg.tasks << ',' << cfg.busy << ',' << cfg.iterations << ',' << phase << ','
        << round << ',' << order << ',' << value.submit_ns << ',' << value.collect_ns << ','
        << value.total_ns << ',' << value.close_ns << ',' << value.checksum << ','
        << value.first_feedback_ns << ',' << value.all_feedback_ns << ','
        << value.release_to_return_ns << ',' << value.future_cleanup_ns << ','
        << value.broken_count << ',' << value.executed_count << '\n';
    csv.flush();
}

void run_suite(std::ofstream& csv, std::vector<config> configs, int rounds,
               std::mt19937& random, std::size_t& order) {
    auto measure = [&](const config& cfg) {
        if (cfg.suite.starts_with("close_")) { return close_sample(cfg); }
        if (cfg.suite == "backlog") { return backlog_sample(cfg); }
        if (cfg.implementation == "paused") { return paused_sample(cfg); }
        return task_sample(cfg);
    };
    for (const auto& cfg : configs) {
        write_sample(csv, cfg, measure(cfg), "warmup", 0, ++order);
    }
    std::cout << configs.front().suite << ": warmup complete" << std::endl;
    for (int round = 1; round <= rounds; ++round) {
        std::shuffle(configs.begin(), configs.end(), random);
        for (const auto& cfg : configs) {
            write_sample(csv, cfg, measure(cfg), "measured", round, ++order);
        }
        std::cout << configs.front().suite << ": round " << round << '/' << rounds
                  << " complete" << std::endl;
    }
}

std::vector<config> task_configs(std::string name, std::size_t tasks,
                                std::uint64_t iterations, work_function work) {
    const auto expected = reference(work, tasks, iterations);
    std::vector<config> result;
    for (const std::size_t workers : {0U, 1U, 2U, 4U, 8U}) {
        result.push_back({name, workers == 0 ? "serial" : "pool", workers,
                          tasks, 0, iterations, work, expected});
    }
    return result;
}
}

int main(int argc, char* argv[]) {
    try {
#ifndef NDEBUG
        throw std::runtime_error("benchmark requires Release/NDEBUG");
#endif
        require(std::string_view(THREAD_POOL_BENCHMARK_BUILD_TYPE) == "Release",
                "benchmark requires CMAKE_BUILD_TYPE=Release");
        require(argc == 3 || argc == 4, "usage: thread_pool_benchmark output.csv rounds [basic|advanced]");
        const std::string mode = argc == 4 ? argv[3] : "basic";
        require(mode == "basic" || mode == "advanced", "unknown benchmark suite");
        const std::string round_text = argv[2];
        std::size_t parsed = 0;
        const int rounds = std::stoi(round_text, &parsed);
        require(parsed == round_text.size() && rounds >= 10, "at least 10 measured rounds required");
        std::ofstream csv(argv[1]);
        require(csv.is_open(), "cannot open CSV output");
        csv.exceptions(std::ios::badbit | std::ios::failbit);
        csv << "suite,implementation,workers,tasks,busy,iterations,phase,round,run_order,"
               "submit_ns,collect_ns,total_ns,close_ns,checksum,first_feedback_ns,all_feedback_ns,"
               "release_to_return_ns,future_cleanup_ns,broken_count,executed_count\n";
        std::cout << "Release; __cplusplus=" << __cplusplus
                  << "; hardware_concurrency=" << std::thread::hardware_concurrency()
                  << "; rounds=" << rounds << "; shuffle_seed=" << shuffle_seed << std::endl;
        std::mt19937 random(shuffle_seed);
        std::size_t order = 0;
        if (mode == "advanced") {
            constexpr std::uint64_t total_iterations = 6553600;
            std::vector<config> scan;
            for (std::uint64_t iterations = 32; iterations <= 65536; iterations *= 2) {
                auto configs = task_configs("grain_" + std::to_string(iterations),
                    total_iterations / iterations, iterations, benchmark_workloads::mix);
                scan.insert(scan.end(), configs.begin(), configs.end());
            }
            run_suite(csv, std::move(scan), rounds, random, order);
            std::vector<config> enqueue, backlog;
            for (const std::size_t tasks : {10000U, 100000U}) {
                for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                    for (const std::string implementation : {"pool", "paused"}) {
                        enqueue.push_back({"enqueue_" + std::to_string(tasks), implementation,
                            workers, tasks, workers, 0, benchmark_workloads::tiny,
                            reference(benchmark_workloads::tiny, tasks, 0)});
                    }
                    backlog.push_back({"backlog", "shutdown_now", workers, tasks, workers, 0,
                        benchmark_workloads::tiny, reference([](std::uint64_t seed, std::uint64_t) {
                            return seed;
                        }, workers, 0)});
                }
            }
            run_suite(csv, std::move(enqueue), rounds, random, order);
            run_suite(csv, std::move(backlog), rounds, random, order);
            std::cout << "All " << order << " samples validated (warmup included)" << std::endl;
            return 0;
        }
        run_suite(csv, task_configs("submit_100k", 100000, 0, benchmark_workloads::tiny), rounds, random, order);
        run_suite(csv, task_configs("throughput_10k", 10000, 32, benchmark_workloads::mix), rounds, random, order);
        run_suite(csv, task_configs("throughput_100k", 100000, 32, benchmark_workloads::mix), rounds, random, order);
        run_suite(csv, task_configs("cpu", 256, 200000, benchmark_workloads::mix), rounds, random, order);
        for (const std::string scenario : {"idle", "partial", "all"}) {
            std::vector<config> configs;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                if (scenario == "partial" && workers == 1) { continue; }
                const auto busy = scenario == "idle" ? 0 : scenario == "partial" ? workers / 2 : workers;
                const auto expected = reference(benchmark_workloads::mix, busy, close_iterations);
                for (const std::string mode : {"shutdown", "shutdown_now"}) {
                    configs.push_back({"close_" + scenario, mode, workers, busy, busy,
                                       close_iterations, benchmark_workloads::mix, expected});
                }
            }
            run_suite(csv, std::move(configs), rounds, random, order);
        }
        std::cout << "All " << order << " samples validated (warmup included)" << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
