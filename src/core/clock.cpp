#include "llt/core/clock.hpp"

#include <fstream>
#include <string>

namespace llt::clk {
namespace {

double g_tsc_ghz = 0.0;

}  // namespace

void clock_init(unsigned window_ms) {
    if (detail::g_ns_per_tick_q32 != 0) return;

    const uint64_t window_ns = static_cast<uint64_t>(window_ms) * 1'000'000ULL;

    const uint64_t t0 = mono_ns();
    const uint64_t c0 = rdtsc_serialized();

    uint64_t t1 = t0;
    while (t1 - t0 < window_ns) t1 = mono_ns();

    const uint64_t c1 = rdtsc_serialized();

    const uint64_t elapsed_ns    = t1 - t0;
    const uint64_t elapsed_ticks = c1 - c0;
    if (elapsed_ticks == 0 || elapsed_ns == 0) return;

    const double ns_per_tick = static_cast<double>(elapsed_ns) / static_cast<double>(elapsed_ticks);

    detail::g_ns_per_tick_q32 = static_cast<uint64_t>(ns_per_tick * 4294967296.0 + 0.5);
    g_tsc_ghz                 = 1.0 / ns_per_tick;
}

double tsc_ghz() noexcept { return g_tsc_ghz; }

bool tsc_is_invariant() {
    std::ifstream f("/proc/cpuinfo");
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("flags", 0) != 0) continue;
        const bool constant = line.find(" constant_tsc") != std::string::npos;
        const bool nonstop  = line.find(" nonstop_tsc") != std::string::npos;
        return constant && nonstop;
    }
    return false;
}

}  // namespace llt::clk
