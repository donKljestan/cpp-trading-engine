// Smoke test and micro-benchmark for the core primitives.
//
// The numbers printed here are the floor of the whole system: no strategy can react
// faster than the time it takes to parse a price and hand it to the next thread.

#include "llt/core/clock.hpp"
#include "llt/core/cpu.hpp"
#include "llt/core/fixed.hpp"
#include "llt/core/spsc_ring.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// Stops the optimiser from deleting work whose result is otherwise unused.
template <typename T>
inline void sink(const T& v) noexcept {
    __asm__ __volatile__("" : : "r,m"(v) : "memory");
}

struct Stats {
    uint64_t p50;
    uint64_t p99;
    uint64_t p999;
    uint64_t max;
};

Stats percentiles(std::vector<uint64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) {
        return samples[static_cast<size_t>(q * static_cast<double>(samples.size() - 1))];
    };
    return Stats{at(0.50), at(0.99), at(0.999), samples.back()};
}

void print_row(const char* name, const Stats& s) {
    std::printf("  %-24s p50 %5lu   p99 %5lu   p99.9 %6lu   max %7lu   (ns)\n", name,
                static_cast<unsigned long>(s.p50), static_cast<unsigned long>(s.p99),
                static_cast<unsigned long>(s.p999), static_cast<unsigned long>(s.max));
}

void bench_rdtsc_overhead() {
    constexpr size_t      kIters = 200'000;
    std::vector<uint64_t> samples;
    samples.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        const uint64_t a = llt::clk::rdtsc_serialized();
        const uint64_t b = llt::clk::rdtsc_serialized();
        samples.push_back(llt::clk::tsc_to_ns(b - a));
    }
    print_row("rdtsc_serialized pair", percentiles(samples));
}

void bench_parse_fixed() {
    // Shapes taken straight from a Binance bookTicker payload.
    const char* const inputs[] = {"67890.10", "0.00012340", "3421.55", "1.00000000",
                                  "98765.43210000"};

    constexpr size_t      kIters = 200'000;
    std::vector<uint64_t> samples;
    samples.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        const char* const s   = inputs[i % 5];
        const size_t      len = std::strlen(s);

        llt::Fixed     f{};
        const uint64_t a  = llt::clk::rdtsc_serialized();
        const bool     ok = llt::parse_fixed(s, s + len, f);
        const uint64_t b  = llt::clk::rdtsc_serialized();

        sink(f);
        sink(ok);
        samples.push_back(llt::clk::tsc_to_ns(b - a));
    }
    print_row("parse_fixed", percentiles(samples));
}

// Round-trip over two rings with exactly one message in flight, halved.
//
// Measuring push-to-pop on a single ring with a free-running producer would report
// queue residency instead: the producer outruns the consumer, the ring backs up, and
// every message waits behind ~1024 others.
void bench_spsc_latency() {
    constexpr size_t              kIters = 100'000;
    llt::SpscRing<uint64_t, 1024> downstream;
    llt::SpscRing<uint64_t, 1024> upstream;
    std::vector<uint64_t>         samples;
    samples.reserve(kIters);

    std::thread echo([&] {
        llt::cpu::set_thread_name("llt-bench-echo");
        static_cast<void>(llt::cpu::pin_to_core(4));
        for (size_t i = 0; i < kIters; ++i) {
            uint64_t v = 0;
            while (!downstream.try_pop(v)) llt::cpu::spin_pause();
            while (!upstream.try_push(v)) llt::cpu::spin_pause();
        }
    });

    for (size_t i = 0; i < kIters; ++i) {
        const uint64_t t0 = llt::clk::rdtsc_serialized();
        while (!downstream.try_push(t0)) llt::cpu::spin_pause();

        uint64_t v = 0;
        while (!upstream.try_pop(v)) llt::cpu::spin_pause();

        const uint64_t t1 = llt::clk::rdtsc_serialized();
        samples.push_back(llt::clk::tsc_to_ns(t1 - t0) / 2);
    }
    echo.join();

    print_row("spsc handoff (1 hop)", percentiles(samples));
}

void bench_spsc_throughput() {
    constexpr size_t              kIters = 5'000'000;
    llt::SpscRing<uint64_t, 1024> q;

    std::thread consumer([&] {
        llt::cpu::set_thread_name("llt-bench-rx");
        static_cast<void>(llt::cpu::pin_to_core(4));
        for (size_t i = 0; i < kIters; ++i) {
            uint64_t v = 0;
            while (!q.try_pop(v)) llt::cpu::spin_pause();
            sink(v);
        }
    });

    const uint64_t t0 = llt::clk::mono_ns();
    for (size_t i = 0; i < kIters; ++i) {
        while (!q.try_push(i)) llt::cpu::spin_pause();
    }
    consumer.join();
    const uint64_t elapsed_ns = llt::clk::mono_ns() - t0;

    const double mps = static_cast<double>(kIters) / (static_cast<double>(elapsed_ns) / 1e9) / 1e6;
    std::printf("  %-24s %.1f M msg/s\n", "spsc throughput", mps);
}

}  // namespace

int main() {
    llt::clk::clock_init();

    std::printf("llt core smoke test\n\n");
    std::printf("  cores           %u\n", llt::cpu::num_cores());
    std::printf("  tsc invariant   %s\n",
                llt::clk::tsc_is_invariant() ? "yes" : "NO - deltas unreliable");
    std::printf("  tsc frequency   %.3f GHz\n\n", llt::clk::tsc_ghz());

    if (!llt::cpu::pin_to_core(2)) {
        std::printf("  warning: could not pin to core 2, timings will be noisier\n\n");
    }

    std::printf("micro-benchmarks\n");
    bench_rdtsc_overhead();
    bench_parse_fixed();
    bench_spsc_latency();
    bench_spsc_throughput();

    std::printf("\n  note: the two rdtsc_serialized calls are inside each timed region,\n");
    std::printf("        so subtract the pair cost to get the true operation cost.\n");
    return 0;
}
