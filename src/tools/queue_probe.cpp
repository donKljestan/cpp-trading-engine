// Measures how fast resting size leaves a price level without being traded.
//
// The backtest's answer turns on this number. When the size at a price falls by more
// than the trades at that price explain, the difference left through cancellation.
//
// The estimate is a LOWER bound, deliberately. Only the net change between snapshots
// is observable, so new orders arriving during the interval mask cancellations that
// really happened. Erring low is the safe direction: if even this bound exceeds the
// rate at which the strategy turns profitable, the pessimistic fill model was wrong.
//
//   queue_probe FILE.mdlog [...] [--symbol TACUSDT]

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "llt/core/clock.hpp"
#include "llt/md/record.hpp"
#include "llt/venue/binance/bn_feed.hpp"
#include "llt/venue/events.hpp"

namespace {

// A snapshot pair separated by more than this is a gap in the feed, not a measurement.
constexpr double kMaxIntervalS = 1.0;

struct Bucket {
    double   cancelled = 0.0;  // size that left without a trade to explain it
    double   traded    = 0.0;  // size that left because it was hit
    double   exposure  = 0.0;  // size-seconds observed, the denominator of a rate
    uint64_t samples   = 0;

    [[nodiscard]] double rate_per_s() const {
        return exposure > 0.0 ? cancelled / exposure : 0.0;
    }
    [[nodiscard]] double trade_share() const {
        const double left = cancelled + traded;
        return left > 0.0 ? 100.0 * traded / left : 0.0;
    }
};

struct SymbolState {
    llt::DepthSnapshot prev{};
    bool               have_prev = false;

    // Trades seen since the previous snapshot, keyed by price.
    std::map<int64_t, int64_t> traded;

    Bucket touch;  // the level we would actually be quoting at
    Bucket near;   // levels 1-4
    Bucket deep;   // everything further out

    std::vector<double> touch_rates;  // kept only for the touch, for a median
};

void observe(Bucket& b, int64_t resting, int64_t decrease, int64_t trade_qty,
             double dt_s) {
    const int64_t explained = std::min(decrease, trade_qty);
    const int64_t cancelled = decrease - explained;

    b.cancelled += static_cast<double>(cancelled);
    b.traded += static_cast<double>(explained);
    b.exposure += static_cast<double>(resting) * dt_s;
    ++b.samples;
}

void scan_side(SymbolState& s, const llt::PriceLevel* before, size_t n_before,
               const llt::PriceLevel* after, size_t n_after, double dt_s) {
    for (size_t i = 0; i < n_before; ++i) {
        const int64_t px      = before[i].px.raw;
        const int64_t resting = before[i].qty.raw;
        if (resting <= 0) continue;

        int64_t remaining = -1;
        for (size_t j = 0; j < n_after; ++j) {
            if (after[j].px.raw == px) {
                remaining = after[j].qty.raw;
                break;
            }
        }
        // Out of the visible window: cannot tell emptied from merely pushed away.
        if (remaining < 0) continue;

        const int64_t decrease = (resting > remaining) ? resting - remaining : 0;
        const auto    hit      = s.traded.find(px);
        const int64_t trade_qty = (hit == s.traded.end()) ? 0 : hit->second;

        Bucket& b = (i == 0) ? s.touch : (i < 5 ? s.near : s.deep);
        observe(b, resting, decrease, trade_qty, dt_s);

        if (i == 0) {
            const int64_t cancelled = decrease - std::min(decrease, trade_qty);
            s.touch_rates.push_back(static_cast<double>(cancelled) /
                                    (static_cast<double>(resting) * dt_s));
        }
    }
}

struct Probe {
    std::map<std::string, SymbolState> symbols;
    std::string                        want;

    [[nodiscard]] bool wanted(std::string_view s) const {
        return want.empty() || s == want;
    }

    void on_book_ticker(const llt::BookTicker&) noexcept {}

    void on_trade(const llt::Trade& t) {
        if (!wanted(t.symbol.view())) return;
        symbols[std::string(t.symbol.view())].traded[t.px.raw] += t.qty.raw;
    }

    void on_depth(const llt::DepthSnapshot& d) {
        if (!wanted(d.symbol.view())) return;
        SymbolState& s = symbols[std::string(d.symbol.view())];

        if (s.have_prev) {
            const uint64_t t1 = s.prev.time.market_ns();
            const uint64_t t2 = d.time.market_ns();
            if (t2 > t1) {
                const double dt_s = static_cast<double>(t2 - t1) / 1e9;
                if (dt_s <= kMaxIntervalS) {
                    scan_side(s, s.prev.bids, s.prev.n_bids, d.bids, d.n_bids, dt_s);
                    scan_side(s, s.prev.asks, s.prev.n_asks, d.asks, d.n_asks, dt_s);
                }
            }
        }

        s.prev      = d;
        s.have_prev = true;
        s.traded.clear();
    }
};

double median(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(mid), v.end());
    return v[mid];
}

}  // namespace

int main(int argc, char** argv) {
    llt::clk::clock_init();

    Probe                    probe;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--symbol" && i + 1 < argc) {
            probe.want = argv[++i];
        } else if (!a.starts_with("--")) {
            files.emplace_back(a);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: queue_probe FILE.mdlog [...] [--symbol SYM]\n");
        return 1;
    }

    llt::binance::FeedStats stats;
    for (const auto& path : files) {
        llt::md::RecordReader reader;
        if (!reader.open(path)) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 1;
        }
        llt::md::RecordHeader hdr;
        std::string_view      payload;
        while (reader.next(hdr, payload)) {
            static_cast<void>(
                llt::binance::dispatch_payload(payload, hdr.recv_tsc, stats, probe));
        }
    }

    if (probe.symbols.empty()) {
        std::fprintf(stderr, "no depth snapshots for the requested symbol\n");
        return 1;
    }

    std::printf("\nqueue drain by cancellation, lower bound (net of new orders)\n\n");
    std::printf("%-12s %10s %9s %9s %9s %11s %9s\n", "symbol", "samples", "touch/s",
                "near/s", "deep/s", "touch med/s", "traded%");
    std::printf("%s\n", std::string(76, '-').c_str());

    for (auto& [name, s] : probe.symbols) {
        if (s.touch.samples == 0) continue;
        std::printf("%-12s %10lu %9.2f %9.2f %9.2f %11.2f %9.1f\n", name.c_str(),
                    static_cast<unsigned long>(s.touch.samples), s.touch.rate_per_s(),
                    s.near.rate_per_s(), s.deep.rate_per_s(), median(s.touch_rates),
                    s.touch.trade_share());
    }

    std::printf("\ntouch/s is the figure the fill model needs: the fraction of the size\n");
    std::printf("resting at the best price that leaves each second without trading.\n");
    std::printf("traded%% is how much of all departing size trades account for; the rest\n");
    std::printf("is cancellation, which the simulator assumed did not happen.\n");
    return 0;
}
