// Measures round-trip time to Binance and the local clock offset, NTP style.
//
//   rtt    = t1 - t0
//   offset = serverTime - (t0 + t1) / 2
//
// Neither figure depends on the local clock being correct, which matters because WSL2
// has no RTC and a fresh cloud instance has not run chrony yet. The offset is reported
// so every other timestamp comparison in the system can be corrected by it.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "llt/core/clock.hpp"
#include "llt/net/http.hpp"
#include "llt/venue/binance/bn_env.hpp"

namespace {

constexpr uint16_t kPort = 443;

bool parse_server_time_ms(std::string_view body, uint64_t& out) {
    const size_t pos = body.find("\"serverTime\":");
    if (pos == std::string_view::npos) return false;

    uint64_t v    = 0;
    bool     seen = false;
    for (size_t i = pos + 13; i < body.size() && body[i] >= '0' && body[i] <= '9'; ++i) {
        v    = v * 10 + static_cast<uint64_t>(body[i] - '0');
        seen = true;
    }
    if (!seen) return false;
    out = v;
    return true;
}

template <typename T>
void report(const char* label, std::vector<T>& v, const char* unit) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) {
        return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
    };
    std::printf("  %-10s min %8.3f   p50 %8.3f   p90 %8.3f   p99 %8.3f   max %8.3f  %s\n",
                label, static_cast<double>(v.front()), static_cast<double>(at(0.50)),
                static_cast<double>(at(0.90)), static_cast<double>(at(0.99)),
                static_cast<double>(v.back()), unit);
}

}  // namespace

int main(int argc, char** argv) {
    llt::clk::clock_init();

    const long samples = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 30;
    const char* const kHost = llt::binance::active_env().rest;

    llt::net::HttpClient http;
    if (!http.connect(kHost, kPort, 10'000)) {
        std::fprintf(stderr, "connect failed: %s\n", llt::net::HttpClient::last_error());
        return 1;
    }

    std::printf("probing https://%s/fapi/v1/time  (%ld samples, keep-alive)\n\n", kHost,
                samples);

    std::vector<double> rtt_ms;
    std::vector<double> offset_ms;
    rtt_ms.reserve(static_cast<size_t>(samples));
    offset_ms.reserve(static_cast<size_t>(samples));

    for (long i = 0; i < samples; ++i) {
        const uint64_t t0_mono = llt::clk::mono_ns();
        const uint64_t t0_wall = llt::clk::wall_ns();

        llt::net::HttpResponse resp;
        if (!http.get("/fapi/v1/time", 10'000, resp)) {
            std::fprintf(stderr, "request failed: %s\n", llt::net::HttpClient::last_error());
            return 1;
        }

        const uint64_t t1_mono = llt::clk::mono_ns();
        const uint64_t t1_wall = llt::clk::wall_ns();

        if (resp.status != 200) {
            std::fprintf(stderr, "HTTP %d: %s\n", resp.status, resp.body.c_str());
            return 1;
        }

        uint64_t server_ms = 0;
        if (!parse_server_time_ms(resp.body, server_ms)) {
            std::fprintf(stderr, "could not parse serverTime from: %s\n", resp.body.c_str());
            return 1;
        }

        const double rtt = static_cast<double>(t1_mono - t0_mono) / 1e6;

        const double midpoint_ms =
            (static_cast<double>(t0_wall) + static_cast<double>(t1_wall)) / 2.0 / 1e6;
        const double offset = static_cast<double>(server_ms) - midpoint_ms;

        rtt_ms.push_back(rtt);
        offset_ms.push_back(offset);
    }

    std::printf("results\n");
    report("rtt", rtt_ms, "ms");
    report("offset", offset_ms, "ms");

    std::sort(offset_ms.begin(), offset_ms.end());
    const double median_offset = offset_ms[offset_ms.size() / 2];

    std::sort(rtt_ms.begin(), rtt_ms.end());
    const double median_rtt = rtt_ms[rtt_ms.size() / 2];

    std::printf("\nlocal clock is %.0f ms %s the exchange\n", std::abs(median_offset),
                median_offset > 0 ? "BEHIND" : "ahead of");
    std::printf("one-way latency is roughly %.1f ms\n\n", median_rtt / 2.0);

    if (median_rtt < 10.0) {
        std::printf("verdict: colocated. tight quoting is on the table.\n");
    } else if (median_rtt < 50.0) {
        std::printf("verdict: same region. market making on mid-cap symbols is viable.\n");
    } else if (median_rtt < 150.0) {
        std::printf("verdict: distant. only wide-spread, slow-moving symbols.\n");
    } else {
        std::printf("verdict: far. resting quotes will be picked off; do not quote from here.\n");
    }

    return 0;
}
