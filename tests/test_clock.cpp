#include "harness.hpp"
#include "llt/core/clock.hpp"
#include "llt/core/cpu.hpp"

#include <thread>

LLT_TEST(clock_calibration_is_plausible) {
    llt::clk::clock_init(20);

    const double ghz = llt::clk::tsc_ghz();
    // Any real x86 part sits well inside this band; outside it the calibration is wrong.
    LLT_CHECK(ghz > 0.5);
    LLT_CHECK(ghz < 10.0);
}

LLT_TEST(clock_tsc_delta_matches_wall_clock) {
    llt::clk::clock_init(20);

    const uint64_t t0 = llt::clk::mono_ns();
    const uint64_t c0 = llt::clk::rdtsc_serialized();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const uint64_t c1 = llt::clk::rdtsc_serialized();
    const uint64_t t1 = llt::clk::mono_ns();

    const uint64_t mono_elapsed = t1 - t0;
    const uint64_t tsc_elapsed  = llt::clk::tsc_to_ns(c1 - c0);

    const double ratio = static_cast<double>(tsc_elapsed) / static_cast<double>(mono_elapsed);
    LLT_CHECK(ratio > 0.98);
    LLT_CHECK(ratio < 1.02);
}

LLT_TEST(clock_sources_are_monotonic) {
    LLT_CHECK(llt::clk::mono_ns() > 0);
    LLT_CHECK(llt::clk::wall_ns() > 0);

    const uint64_t a = llt::clk::mono_ns();
    const uint64_t b = llt::clk::mono_ns();
    LLT_CHECK(b >= a);
}

LLT_TEST(cpu_affinity_roundtrip) {
    LLT_CHECK(llt::cpu::num_cores() >= 1);
    LLT_CHECK(!llt::cpu::pin_to_core(llt::cpu::num_cores() + 100));

    // Run on a worker so the test process itself is not left pinned.
    bool pinned  = false;
    int  on_core = -1;
    std::thread t([&] {
        pinned  = llt::cpu::pin_to_core(0);
        on_core = llt::cpu::current_core();
    });
    t.join();

    LLT_CHECK(pinned);
    LLT_CHECK_EQ(on_core, 0);
}
