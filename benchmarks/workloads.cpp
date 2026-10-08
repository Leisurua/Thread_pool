#include "workloads.hpp"

namespace benchmark_workloads {
// 独立编译单元、禁用本目标 IPO，并禁止内联；所有返回值由调用方校验。
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
std::uint64_t tiny(std::uint64_t seed, std::uint64_t) {
    return (seed ^ 0x9e3779b97f4a7c15ULL) * 0xbf58476d1ce4e5b9ULL + 17;
}

#if defined(__GNUC__)
[[gnu::noinline]]
#endif
std::uint64_t mix(std::uint64_t seed, std::uint64_t iterations) {
    for (std::uint64_t index = 0; index < iterations; ++index) {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        seed *= 0x2545f4914f6cdd1dULL;
    }
    return seed;
}
}
