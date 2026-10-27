#pragma once

// Timestamping. Three different clocks, deliberately not interchangeable:
//
//   rdtsc()    - CPU cycle counter. Cheapest source available (~6ns, no vDSO call).
//                Meaningful only as a delta. Use for intra-process latency measurement.
//   mono_ns()  - CLOCK_MONOTONIC_RAW. Never steps, unaffected by NTP. Use for timeouts.
//   wall_ns()  - CLOCK_REALTIME. The only clock comparable to an exchange timestamp,
//                and therefore the only one worth disciplining with chrony.

#include <cstdint>
#include <ctime>

#if defined(__x86_64__) || defined(__i386__)
#    define LLT_X86 1
#    include <x86intrin.h>
#else
#    define LLT_X86 0
#endif

namespace llt::clk {

namespace detail {
// Q32 fixed-point nanoseconds-per-tick, published by clock_init().
inline uint64_t g_ns_per_tick_q32 = 0;
}  // namespace detail

[[nodiscard]] inline uint64_t rdtsc() noexcept {
#if LLT_X86
    return __rdtsc();
#else
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

// rdtsc cannot be reordered against surrounding loads; this variant can, at the
// cost of a serialising fence. Use it around the code you are actually timing.
[[nodiscard]] inline uint64_t rdtsc_serialized() noexcept {
#if LLT_X86
    unsigned       aux = 0;
    const uint64_t t   = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    return rdtsc();
#endif
}

[[nodiscard]] inline uint64_t mono_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

[[nodiscard]] inline uint64_t wall_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

// Converts a TSC delta to nanoseconds. Returns 0 until clock_init() has run.
[[nodiscard]] inline uint64_t tsc_to_ns(uint64_t delta) noexcept {
    return static_cast<uint64_t>((static_cast<__uint128_t>(delta) * detail::g_ns_per_tick_q32) >> 32);
}

// Calibrates the TSC against CLOCK_MONOTONIC_RAW. Must be called once at startup
// before tsc_to_ns(); idempotent. Blocks for roughly `window_ms`.
void clock_init(unsigned window_ms = 20);

// True when the CPU advertises constant_tsc and nonstop_tsc. Without both, the TSC
// drifts with frequency scaling and halts in deep C-states, making deltas meaningless.
[[nodiscard]] bool tsc_is_invariant();

[[nodiscard]] double tsc_ghz() noexcept;

}  // namespace llt::clk
