// Replays a recording through the live decode path and summarises it.
//
// Running the same dispatch as the live feed is the point: if replay used its own
// decoder, a backtest would be measuring a program that never trades.
//
//   md_replay file.mdlog [file2.mdlog ...]

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "llt/core/clock.hpp"
#include "llt/md/record.hpp"
#include "llt/venue/binance/bn_feed.hpp"

namespace {

struct SymbolSummary {
    uint64_t            quotes = 0;
    uint64_t            depths = 0;
    uint64_t            trades = 0;
    std::vector<double> spread_bps;
    double              last_mid = 0.0;
};

struct Summary {
    std::map<std::string, SymbolSummary> by_symbol;

    void on_book_ticker(const llt::BookTicker& bt) {
        SymbolSummary& s = by_symbol[std::string(bt.symbol.view())];
        ++s.quotes;
        const double mid = bt.mid().to_double();
        if (mid > 0.0) {
            s.last_mid = mid;
            s.spread_bps.push_back(bt.spread().to_double() / mid * 1e4);
        }
    }

    void on_depth(const llt::DepthSnapshot& d) {
        ++by_symbol[std::string(d.symbol.view())].depths;
    }

    void on_trade(const llt::Trade& t) {
        ++by_symbol[std::string(t.symbol.view())].trades;
    }
};

double median(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

}  // namespace

int main(int argc, char** argv) {
    llt::clk::clock_init();

    if (argc < 2) {
        std::fprintf(stderr, "usage: md_replay file.mdlog [...]\n");
        return 1;
    }

    Summary              summary;
    llt::binance::FeedStats stats;

    uint64_t total_records = 0;
    uint64_t first_wall_ns = 0;
    uint64_t last_wall_ns  = 0;
    uint64_t gaps          = 0;
    uint64_t prev_wall_ns  = 0;

    for (int i = 1; i < argc; ++i) {
        llt::md::RecordReader reader;
        if (!reader.open(argv[i])) {
            std::fprintf(stderr, "cannot read %s (missing, or not an llt recording)\n",
                         argv[i]);
            return 1;
        }

        llt::md::RecordHeader hdr;
        std::string_view      payload;
        uint64_t              in_file = 0;

        while (reader.next(hdr, payload)) {
            if (first_wall_ns == 0) first_wall_ns = hdr.recv_wall_ns;
            // A silence longer than a few seconds is a disconnect, not a quiet market.
            if (prev_wall_ns != 0 && hdr.recv_wall_ns > prev_wall_ns + 5'000'000'000ULL) {
                ++gaps;
            }
            prev_wall_ns = hdr.recv_wall_ns;
            last_wall_ns = hdr.recv_wall_ns;

            ++stats.frames;
            ++in_file;
            static_cast<void>(
                llt::binance::dispatch_payload(payload, hdr.recv_tsc, stats, summary));
        }

        total_records += in_file;
        std::printf("%-40s %8lu records%s\n", argv[i],
                    static_cast<unsigned long>(in_file),
                    reader.truncated() ? "  [TRUNCATED TAIL]" : "");
    }

    const double span_s =
        (last_wall_ns > first_wall_ns)
            ? static_cast<double>(last_wall_ns - first_wall_ns) / 1e9
            : 0.0;

    std::printf("\n%lu records over %.1f s (%.1f min)\n",
                static_cast<unsigned long>(total_records), span_s, span_s / 60.0);
    std::printf("decoded: bookTicker %lu   depth %lu   trades %lu\n",
                static_cast<unsigned long>(stats.book_tickers),
                static_cast<unsigned long>(stats.depths),
                static_cast<unsigned long>(stats.trades));
    std::printf("ignored %lu   decode errors %lu   gaps over 5s: %lu\n",
                static_cast<unsigned long>(stats.ignored),
                static_cast<unsigned long>(stats.decode_errors),
                static_cast<unsigned long>(gaps));

    std::printf("\n%-14s %9s %8s %8s %12s %10s\n", "symbol", "quotes", "depths", "trades",
                "spread_bps", "last");
    std::printf("%s\n", std::string(66, '-').c_str());

    for (auto& [name, s] : summary.by_symbol) {
        std::printf("%-14s %9lu %8lu %8lu %12.3f %10.6g\n", name.c_str(),
                    static_cast<unsigned long>(s.quotes),
                    static_cast<unsigned long>(s.depths),
                    static_cast<unsigned long>(s.trades), median(s.spread_bps),
                    s.last_mid);
    }

    return 0;
}
