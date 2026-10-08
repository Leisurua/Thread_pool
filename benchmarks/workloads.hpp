#ifndef THREAD_POOL_BENCHMARK_WORKLOADS_HPP
#define THREAD_POOL_BENCHMARK_WORKLOADS_HPP
#include <cstdint>

namespace benchmark_workloads {
std::uint64_t tiny(std::uint64_t seed, std::uint64_t iterations);
std::uint64_t mix(std::uint64_t seed, std::uint64_t iterations);
}
#endif
