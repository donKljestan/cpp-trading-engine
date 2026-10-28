// Connects to the Binance USDⓈ-M futures market data feed and reports what arrives.
//
//   md_dump [streams] [messages]
//   md_dump btcusdt@bookTicker 100
//   md_dump "btcusdt@bookTicker/btcusdt@depth20@100ms/btcusdt@trade" 300
//
// LLT_RAW=1 dumps undecoded frames; LLT_SUB sends a live subscription request first.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "llt/core/clock.hpp"
#include "llt/net/ws.hpp"
#include "llt/venue/binance/bn_feed.hpp"
#include "llt/venue/binance/bn_rest.hpp"

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t                   begin = 0;
    while (begin <= s.size()) {
        const size_t end = s.find(sep, begin);
        if (end == std::string::npos) {
            out.push_back(s.substr(begin));
            break;
        }
        out.push_back(s.substr(begin, end - begin));
        begin = end + 1;
    }
    return out;
}

char* format(llt::Fixed v, char* buf, size_t cap) {
    const size_t n = llt::format_fixed(v, buf, cap - 1);
    buf[n]         = '\0';
    return buf;
}

struct Printer {
    long                shown = 0;
    long                limit = 6;
    std::vector<double> spread_bps;

    void on_book_ticker(const llt::BookTicker& bt) {
        const double mid = bt.mid().to_double();
        if (mid > 0.0) spread_bps.push_back(bt.spread().to_double() / mid * 1e4);

        if (shown++ >= limit) return;
        char a[32];
        char b[32];
        char c[32];
        char d[32];
        std::printf("  book  %-12s %s x %s  |  %s x %s\n", bt.symbol.c_str(),
                    format(bt.bid_qty, a, sizeof(a)), format(bt.bid_px, b, sizeof(b)),
                    format(bt.ask_px, c, sizeof(c)), format(bt.ask_qty, d, sizeof(d)));
    }

    void on_depth(const llt::DepthSnapshot& snap) {
        if (shown++ >= limit) return;
        std::printf("  depth %-12s %u bids / %u asks   imbalance(5) %+.3f\n",
                    snap.symbol.c_str(), snap.n_bids, snap.n_asks, snap.imbalance(5));
    }

    void on_trade(const llt::Trade& t) {
        if (shown++ >= limit) return;
        char p[32];
        char q[32];
        std::printf("  trade %-12s %s @ %s   aggressor %s\n", t.symbol.c_str(),
                    format(t.qty, q, sizeof(q)), format(t.px, p, sizeof(p)),
                    t.aggressor() == llt::Side::Buy ? "buy" : "sell");
    }
};

void report_spreads(std::vector<double>& v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) {
        return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
    };

    std::printf("\nquoted spread (basis points)\n");
    std::printf("  min %.4f   p50 %.4f   p90 %.4f   max %.4f\n", v.front(), at(0.50),
                at(0.90), v.back());
    std::printf("  maker round trip at VIP0 costs 4.0 bps; a spread below that cannot pay.\n");
}

// Prints frames exactly as the exchange sends them, bypassing all decoding.
// The only way to tell "we failed to parse it" apart from "it never arrived".
// An argument starting with '/' is used as the raw request path, so any endpoint
// shape can be probed, not just /stream?streams=.
int raw_mode(const std::string& streams, long want) {
    const std::string path =
        streams.starts_with('/') ? streams : ("/stream?streams=" + streams);
    const char* const host = llt::binance::active_env().md_ws;
    std::printf("raw mode: wss://%s%s\n\n", host, path.c_str());

    llt::net::WsClient ws;
    if (!ws.connect(host, 443, path.c_str(), 10'000)) {
        std::fprintf(stderr, "connect failed: %s\n", llt::net::WsClient::last_error());
        return 1;
    }

    bool deadline_reached = false;

    // Optional live subscription request, e.g.
    //   LLT_SUB='{"method":"SUBSCRIBE","params":["btcusdt@aggTrade"],"id":1}'
    if (const char* sub = std::getenv("LLT_SUB"); sub != nullptr) {
        std::printf("sending: %s\n\n", sub);
        if (!ws.send_text(sub)) {
            std::fprintf(stderr, "subscribe send failed: %s\n",
                         llt::net::WsClient::last_error());
            return 1;
        }
    }
    const uint64_t deadline = llt::clk::mono_ns() + 20'000'000'000ULL;
    long           seen     = 0;
    long           n_binary = 0;
    long           n_ping   = 0;
    long           n_idle   = 0;

    while (seen < want && llt::clk::mono_ns() < deadline) {
        std::string_view payload;

        switch (ws.poll(payload)) {
            case llt::net::WsClient::Event::Text:
                std::printf("[%ld text %zu] %.*s\n", seen, payload.size(),
                            static_cast<int>(payload.size()), payload.data());
                ++seen;
                break;

            case llt::net::WsClient::Event::Binary:
                std::printf("[%ld BINARY %zu bytes]\n", seen, payload.size());
                ++n_binary;
                ++seen;
                break;

            case llt::net::WsClient::Event::Ping:
                ++n_ping;
                break;

            case llt::net::WsClient::Event::Pong:
                break;

            case llt::net::WsClient::Event::None:
                ++n_idle;
                break;

            case llt::net::WsClient::Event::Close:
                std::printf("[closed by server]\n");
                deadline_reached = true;
                break;

            case llt::net::WsClient::Event::Error:
                std::fprintf(stderr, "error: %s\n", llt::net::WsClient::last_error());
                return 1;
        }
        if (deadline_reached) break;
    }

    std::printf("\nframes %ld (binary %ld)   pings %ld   idle polls %ld\n", seen, n_binary,
                n_ping, n_idle);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    llt::clk::clock_init();

    const std::string streams = (argc > 1) ? argv[1] : "btcusdt@bookTicker";
    const long        want    = (argc > 2) ? std::strtol(argv[2], nullptr, 10) : 100;

    if (std::getenv("LLT_RAW") != nullptr) return raw_mode(streams, want);

    llt::binance::RestClient rest;
    if (rest.connect()) {
        std::vector<llt::Instrument> instruments;
        if (rest.fetch_instruments(instruments)) {
            size_t tradable = 0;
            for (const auto& i : instruments) tradable += i.tradable ? 1 : 0;
            std::printf("exchangeInfo: %zu USDT perpetuals, %zu tradable\n",
                        instruments.size(), tradable);

            for (const auto& i : instruments) {
                if (i.symbol.view() != "BTCUSDT") continue;
                char t[32];
                char s[32];
                char n[32];
                std::printf("  BTCUSDT tick %s  step %s  minNotional %s\n",
                            format(i.tick_size, t, sizeof(t)),
                            format(i.step_size, s, sizeof(s)),
                            format(i.min_notional, n, sizeof(n)));
            }
        } else {
            std::fprintf(stderr, "exchangeInfo failed: %s\n",
                         llt::binance::RestClient::last_error());
        }
        rest.close();
    }

    llt::binance::MdFeed::Config cfg;
    cfg.streams = split(streams, '/');

    llt::binance::MdFeed feed;
    if (!feed.connect(cfg)) {
        std::fprintf(stderr, "feed connect failed: %s\n",
                     llt::binance::MdFeed::last_error());
        return 1;
    }
    std::printf("\nstreaming %s\n\n", streams.c_str());

    Printer printer;
    bool    running = true;

    while (running && static_cast<long>(feed.stats().frames) < want) {
        switch (feed.poll(printer)) {
            case llt::binance::MdFeed::PollResult::Idle:
            case llt::binance::MdFeed::PollResult::Delivered:
                break;
            case llt::binance::MdFeed::PollResult::Disconnected:
                std::printf("\n[server closed the connection]\n");
                running = false;
                break;
            case llt::binance::MdFeed::PollResult::Error:
                std::fprintf(stderr, "\nstream error: %s\n",
                             llt::binance::MdFeed::last_error());
                return 1;
        }
    }

    const auto& st = feed.stats();
    std::printf("\nframes %lu   bookTicker %lu   depth %lu   trades %lu\n",
                static_cast<unsigned long>(st.frames),
                static_cast<unsigned long>(st.book_tickers),
                static_cast<unsigned long>(st.depths),
                static_cast<unsigned long>(st.trades));
    std::printf("pings %lu   ignored %lu   decode errors %lu\n",
                static_cast<unsigned long>(st.pings),
                static_cast<unsigned long>(st.ignored),
                static_cast<unsigned long>(st.decode_errors));

    report_spreads(printer.spread_bps);
    return 0;
}
