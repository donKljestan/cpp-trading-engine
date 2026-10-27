// Connects to the Binance USDⓈ-M futures market data stream and reports what arrives.
//
// This is the first end-to-end proof of the network stack, and the seed of lat_probe:
// the delay column is the only number that decides whether a quoting strategy is
// viable from a given location.
//
//   md_dump [streams] [messages]
//   md_dump btcusdt@bookTicker 50
//   md_dump "btcusdt@bookTicker/ethusdt@aggTrade" 200

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "llt/core/clock.hpp"
#include "llt/net/ws.hpp"

namespace {

constexpr const char* kHost = "fstream.binance.com";
constexpr uint16_t    kPort = 443;

// Pulls an unsigned integer field out of a JSON object without a full parse.
// Good enough for a diagnostic tool; the hot path gets a real scanner in bn_parse.
bool find_uint_field(std::string_view json, std::string_view key, uint64_t& out) {
    char pattern[32];
    const int n = std::snprintf(pattern, sizeof(pattern), "\"%.*s\":",
                                static_cast<int>(key.size()), key.data());
    if (n <= 0) return false;

    const size_t pos = json.find(std::string_view(pattern, static_cast<size_t>(n)));
    if (pos == std::string_view::npos) return false;

    size_t i = pos + static_cast<size_t>(n);
    if (i < json.size() && json[i] == '"') ++i;

    uint64_t v    = 0;
    bool     seen = false;
    for (; i < json.size() && json[i] >= '0' && json[i] <= '9'; ++i) {
        v = v * 10 + static_cast<uint64_t>(json[i] - '0');
        seen = true;
    }
    if (!seen) return false;
    out = v;
    return true;
}

void print_percentiles(std::vector<int64_t>& v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) {
        return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
    };
    std::printf("\ndelay exchange -> local (ms, includes clock offset)\n");
    std::printf("  p50 %ld   p90 %ld   p99 %ld   min %ld   max %ld   n=%zu\n",
                static_cast<long>(at(0.50)), static_cast<long>(at(0.90)),
                static_cast<long>(at(0.99)), static_cast<long>(v.front()),
                static_cast<long>(v.back()), v.size());
}

}  // namespace

int main(int argc, char** argv) {
    llt::clk::clock_init();

    const std::string streams = (argc > 1) ? argv[1] : "btcusdt@bookTicker";
    const long        want    = (argc > 2) ? std::strtol(argv[2], nullptr, 10) : 50;

    const std::string path = "/stream?streams=" + streams;

    std::printf("connecting wss://%s%s\n", kHost, path.c_str());

    const uint64_t t_start = llt::clk::mono_ns();

    llt::net::WsClient ws;
    if (!ws.connect(kHost, kPort, path.c_str(), 10'000)) {
        std::fprintf(stderr, "connect failed: %s\n", llt::net::WsClient::last_error());
        return 1;
    }

    std::printf("connected in %.1f ms\n\n",
                static_cast<double>(llt::clk::mono_ns() - t_start) / 1e6);

    std::vector<int64_t> delays;
    delays.reserve(static_cast<size_t>(want));

    long received = 0;
    while (received < want) {
        std::string_view payload;
        const auto       ev = ws.poll(payload);

        switch (ev) {
            case llt::net::WsClient::Event::None:
                continue;

            case llt::net::WsClient::Event::Text: {
                const uint64_t local_ms = llt::clk::wall_ns() / 1'000'000ULL;

                uint64_t event_ms = 0;
                if (find_uint_field(payload, "E", event_ms)) {
                    delays.push_back(static_cast<int64_t>(local_ms) -
                                     static_cast<int64_t>(event_ms));
                }

                if (received < 5) {
                    std::printf("%.*s\n", static_cast<int>(std::min<size_t>(payload.size(), 400)),
                                payload.data());
                }
                ++received;
                break;
            }

            case llt::net::WsClient::Event::Ping:
                std::printf("[ping, answered]\n");
                break;

            case llt::net::WsClient::Event::Pong:
                break;

            case llt::net::WsClient::Event::Close:
                std::printf("[server closed the connection]\n");
                received = want;
                break;

            case llt::net::WsClient::Event::Binary:
                break;

            case llt::net::WsClient::Event::Error:
                std::fprintf(stderr, "stream error: %s\n", llt::net::WsClient::last_error());
                return 1;
        }
    }

    std::printf("\nreceived %ld messages\n", received);
    print_percentiles(delays);

    static_cast<void>(ws.send_close());
    return 0;
}
