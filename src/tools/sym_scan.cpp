// Ranks every USDⓈ-M perpetual by whether its quoted spread can pay for a round trip.
//
// This is the gate for the whole project. If nothing clears the fee hurdle at a size
// the account can afford, no amount of latency engineering makes market making work,
// and the honest move is to change strategy rather than keep optimising.
//
// Two stages, because the wide stream lies about activity:
//   1. !bookTicker covers every symbol at once, but the exchange throttles it to about
//      one update per symbol per five seconds. Good enough to estimate a spread and
//      reject the obviously unaffordable; useless for measuring how busy a symbol is.
//   2. Dedicated @bookTicker and @trade streams for the survivors, which give the real
//      update rate, the real spread distribution, and how often the symbol prints.
//
//   sym_scan [--secs 45] [--deep-secs 60] [--deep 30] [--budget 20]
//            [--leverage 5] [--fee-bps 4] [--min-vol 1000000] [--csv out.csv]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "llt/core/clock.hpp"
#include "llt/venue/binance/bn_feed.hpp"
#include "llt/venue/binance/bn_rest.hpp"

namespace {

struct Options {
    int         secs      = 45;
    int         deep_secs = 60;
    int         deep      = 30;
    double      budget    = 20.0;
    double      leverage  = 5.0;
    double      fee_bps   = 4.0;  // VIP0 maker, both sides, no BNB discount
    double      min_vol   = 1'000'000.0;
    std::string csv;
    std::string deep_csv;
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const char* const      val  = argv[i + 1];
        if (flag == "--secs") {
            o.secs = static_cast<int>(std::strtol(val, nullptr, 10));
        } else if (flag == "--deep-secs") {
            o.deep_secs = static_cast<int>(std::strtol(val, nullptr, 10));
        } else if (flag == "--deep") {
            o.deep = static_cast<int>(std::strtol(val, nullptr, 10));
        } else if (flag == "--budget") {
            o.budget = std::strtod(val, nullptr);
        } else if (flag == "--leverage") {
            o.leverage = std::strtod(val, nullptr);
        } else if (flag == "--fee-bps") {
            o.fee_bps = std::strtod(val, nullptr);
        } else if (flag == "--min-vol") {
            o.min_vol = std::strtod(val, nullptr);
        } else if (flag == "--csv") {
            o.csv = val;
        } else if (flag == "--deep-csv") {
            o.deep_csv = val;
        }
    }
    return o;
}

struct Samples {
    static constexpr size_t kMax = 20'000;

    std::vector<double> spread_bps;
    uint64_t            quotes = 0;
    uint64_t            trades = 0;
    double              mid    = 0.0;
};

struct Collector {
    std::unordered_map<std::string, Samples> by_symbol;

    void on_book_ticker(const llt::BookTicker& bt) {
        const double mid = bt.mid().to_double();
        if (mid <= 0.0) return;

        Samples& s = by_symbol[std::string(bt.symbol.view())];
        ++s.quotes;
        s.mid = mid;
        if (s.spread_bps.size() < Samples::kMax) {
            s.spread_bps.push_back(bt.spread().to_double() / mid * 1e4);
        }
    }

    void on_trade(const llt::Trade& t) { ++by_symbol[std::string(t.symbol.view())].trades; }
    void on_depth(const llt::DepthSnapshot&) {}
};

bool sample_streams(const std::vector<std::string>& streams, int secs, Collector& out) {
    llt::binance::MdFeed::Config cfg;
    cfg.streams = streams;

    llt::binance::MdFeed feed;
    if (!feed.connect(cfg)) {
        std::fprintf(stderr, "feed connect failed: %s\n", llt::binance::MdFeed::last_error());
        return false;
    }

    const uint64_t deadline =
        llt::clk::mono_ns() + static_cast<uint64_t>(secs) * uint64_t{1'000'000'000};

    while (llt::clk::mono_ns() < deadline) {
        const auto r = feed.poll(out);
        if (r == llt::binance::MdFeed::PollResult::Disconnected ||
            r == llt::binance::MdFeed::PollResult::Error) {
            std::fprintf(stderr, "stream ended early: %s\n",
                         llt::binance::MdFeed::last_error());
            break;
        }
    }
    feed.close();
    return true;
}

double median(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

double percentile(std::vector<double>& v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

struct Row {
    std::string symbol;
    double      spread_bps    = 0.0;
    double      edge_bps      = 0.0;
    double      quote_volume  = 0.0;
    double      min_notional  = 0.0;
    double      margin_needed = 0.0;
    bool        fits_budget   = false;
    bool        pays_fees     = false;
    bool        liquid        = false;
};

}  // namespace

int main(int argc, char** argv) {
    llt::clk::clock_init();
    const Options opt = parse_args(argc, argv);

    llt::binance::RestClient rest;
    if (!rest.connect()) {
        std::fprintf(stderr, "rest connect failed: %s\n",
                     llt::binance::RestClient::last_error());
        return 1;
    }

    std::vector<llt::Instrument> instruments;
    std::vector<llt::binance::Ticker24h> tickers;
    if (!rest.fetch_instruments(instruments) || !rest.fetch_24h_tickers(tickers)) {
        std::fprintf(stderr, "rest fetch failed: %s\n",
                     llt::binance::RestClient::last_error());
        return 1;
    }
    rest.close();

    std::unordered_map<std::string, const llt::Instrument*> by_name;
    for (const auto& i : instruments) {
        if (i.tradable) by_name[std::string(i.symbol.view())] = &i;
    }

    std::unordered_map<std::string, const llt::binance::Ticker24h*> stats;
    for (const auto& t : tickers) stats[std::string(t.symbol.view())] = &t;

    std::printf("%zu tradable USDT perpetuals\n", by_name.size());
    std::printf("stage 1: !bookTicker, %ds (throttled by the venue, spread only)\n",
                opt.secs);

    Collector wide;
    if (!sample_streams({"!bookTicker"}, opt.secs, wide)) return 1;

    if (wide.by_symbol.empty()) {
        std::fprintf(stderr, "no updates received on !bookTicker\n");
        return 1;
    }

    std::vector<Row> rows;
    rows.reserve(wide.by_symbol.size());

    for (auto& [name, s] : wide.by_symbol) {
        const auto inst_it = by_name.find(name);
        if (inst_it == by_name.end()) continue;

        Row r;
        r.symbol     = name;
        r.spread_bps = median(s.spread_bps);
        r.edge_bps   = r.spread_bps - opt.fee_bps;

        const auto stat_it = stats.find(name);
        r.quote_volume = (stat_it != stats.end()) ? stat_it->second->quote_volume : 0.0;

        // An order must clear both the notional floor and the lot-size floor.
        const llt::Instrument& inst = *inst_it->second;
        r.min_notional =
            std::max(inst.min_notional.to_double(), inst.min_qty.to_double() * s.mid);

        r.margin_needed = 2.0 * r.min_notional / opt.leverage;
        r.fits_budget   = r.margin_needed <= opt.budget;
        r.pays_fees     = r.edge_bps > 0.0;
        r.liquid        = r.quote_volume >= opt.min_vol;

        rows.push_back(std::move(r));
    }

    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.edge_bps > b.edge_bps; });

    size_t n_budget = 0;
    size_t n_fees   = 0;
    size_t n_all    = 0;
    for (const auto& r : rows) {
        if (r.fits_budget) ++n_budget;
        if (r.fits_budget && r.pays_fees) ++n_fees;
        if (r.fits_budget && r.pays_fees && r.liquid) ++n_all;
    }

    std::printf("\nbudget $%.2f at %gx   fees %.1f bps   min 24h vol $%.0fM\n",
                opt.budget, opt.leverage, opt.fee_bps, opt.min_vol / 1e6);
    std::printf("funnel over %zu symbols: %zu affordable -> %zu beat fees -> %zu liquid\n",
                rows.size(), n_budget, n_fees, n_all);

    if (n_all == 0) {
        std::printf("\nVERDICT: nothing clears the hurdle. market making is not the "
                    "strategy here.\n");
        return 0;
    }

    std::vector<std::string> shortlist;
    std::vector<std::string> deep_streams;
    for (const auto& r : rows) {
        if (!r.fits_budget || !r.pays_fees || !r.liquid) continue;
        if (static_cast<int>(shortlist.size()) >= opt.deep) break;
        shortlist.push_back(r.symbol);
        deep_streams.push_back(to_lower(r.symbol) + "@bookTicker");
        deep_streams.push_back(to_lower(r.symbol) + "@trade");
    }

    std::printf("\nstage 2: %zu symbols on dedicated streams, %ds\n", shortlist.size(),
                opt.deep_secs);

    Collector deep;
    if (!sample_streams(deep_streams, opt.deep_secs, deep)) return 1;

    std::printf("\n%-14s %10s %9s %9s %8s %9s %9s\n", "symbol", "spread_bps", "p90_bps",
                "edge_bps", "quotes/s", "trades/m", "margin");
    std::printf("%s\n", std::string(74, '-').c_str());

    struct DeepRow {
        std::string symbol;
        double      med = 0.0;
        double      p90 = 0.0;
        double      edge = 0.0;
        double      qps = 0.0;
        double      tpm = 0.0;
        double      margin = 0.0;
    };

    std::vector<DeepRow> deep_rows;
    for (const auto& name : shortlist) {
        const auto it = deep.by_symbol.find(name);
        if (it == deep.by_symbol.end()) continue;

        DeepRow d;
        d.symbol = name;
        d.med    = median(it->second.spread_bps);
        d.p90    = percentile(it->second.spread_bps, 0.90);
        d.edge   = d.med - opt.fee_bps;
        d.qps    = static_cast<double>(it->second.quotes) / static_cast<double>(opt.deep_secs);
        d.tpm = static_cast<double>(it->second.trades) * 60.0 / static_cast<double>(opt.deep_secs);

        for (const auto& r : rows) {
            if (r.symbol == name) {
                d.margin = r.margin_needed;
                break;
            }
        }
        deep_rows.push_back(d);
    }

    std::sort(deep_rows.begin(), deep_rows.end(),
              [](const DeepRow& a, const DeepRow& b) { return a.edge > b.edge; });

    for (const auto& d : deep_rows) {
        std::printf("%-14s %10.3f %9.3f %9.3f %8.2f %9.1f %9.2f\n", d.symbol.c_str(), d.med,
                    d.p90, d.edge, d.qps, d.tpm, d.margin);
    }

    size_t still_good = 0;
    for (const auto& d : deep_rows) still_good += (d.edge > 0.0) ? 1 : 0;

    std::printf("\n%zu of %zu survive the deeper measurement\n", still_good,
                deep_rows.size());
    std::printf("a wide spread is payment for inventory risk, not free money.\n");
    std::printf("trades/m is the ceiling on fills: a symbol that prints twice a minute\n");
    std::printf("cannot fill a two-sided quote often enough to matter.\n");

    if (!opt.deep_csv.empty()) {
        std::FILE* f = std::fopen(opt.deep_csv.c_str(), "w");
        if (f == nullptr) {
            std::fprintf(stderr, "could not write %s\n", opt.deep_csv.c_str());
            return 1;
        }
        std::fprintf(f, "symbol,spread_bps,p90_bps,edge_bps,quotes_per_s,trades_per_min,"
                        "margin_needed\n");
        for (const auto& d : deep_rows) {
            std::fprintf(f, "%s,%.6f,%.6f,%.6f,%.3f,%.2f,%.4f\n", d.symbol.c_str(), d.med,
                         d.p90, d.edge, d.qps, d.tpm, d.margin);
        }
        std::fclose(f);
        std::printf("wrote %zu deep rows to %s\n", deep_rows.size(), opt.deep_csv.c_str());
    }

    if (!opt.csv.empty()) {
        std::FILE* f = std::fopen(opt.csv.c_str(), "w");
        if (f == nullptr) {
            std::fprintf(stderr, "could not write %s\n", opt.csv.c_str());
            return 1;
        }
        std::fprintf(f, "symbol,spread_bps,edge_bps,quote_volume,min_notional,"
                        "margin_needed,fits_budget,pays_fees,liquid\n");
        for (const auto& r : rows) {
            std::fprintf(f, "%s,%.6f,%.6f,%.2f,%.4f,%.4f,%d,%d,%d\n", r.symbol.c_str(),
                         r.spread_bps, r.edge_bps, r.quote_volume, r.min_notional,
                         r.margin_needed, r.fits_budget ? 1 : 0, r.pays_fees ? 1 : 0,
                         r.liquid ? 1 : 0);
        }
        std::fclose(f);
        std::printf("\nwrote %zu rows to %s\n", rows.size(), opt.csv.c_str());
    }

    return 0;
}
