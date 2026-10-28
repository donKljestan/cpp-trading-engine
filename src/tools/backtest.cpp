// Replays a recording through the quoter and a fill simulator.
//
// The number that decides whether the strategy is viable is not the PnL line, which at
// this size is cents either way. It is adverse selection: if the mid consistently
// moves against a fill in the second after it happens, the quotes are being picked off
// and no parameter tuning fixes that.
//
//   backtest FILE.mdlog [--symbol TACUSDT] [--qty 50] [--max-pos 200]
//            [--fee-bps 4] [--buffer-bps 1] [--vol-mult 1.0] [--gamma 1.0]
//            [--latency-us 2000]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "llt/core/clock.hpp"
#include "llt/md/record.hpp"
#include "llt/strat/sim.hpp"
#include "llt/venue/binance/bn_feed.hpp"
#include "llt/venue/binance/bn_rest.hpp"

namespace {

struct Options {
    std::string symbol     = "";
    double      qty        = 0.0;  // 0 means derive from min notional
    double      max_pos    = 0.0;
    double      fee_bps    = 4.0;
    double      buffer_bps = 1.0;
    double      vol_mult   = 1.0;
    double      gamma      = 1.0;
    double      horizon_s  = 5.0;
    uint64_t    latency_us = 2000;
    std::vector<std::string> files;
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };

        if (a == "--symbol") {
            o.symbol = next();
        } else if (a == "--qty") {
            o.qty = std::strtod(next(), nullptr);
        } else if (a == "--max-pos") {
            o.max_pos = std::strtod(next(), nullptr);
        } else if (a == "--fee-bps") {
            o.fee_bps = std::strtod(next(), nullptr);
        } else if (a == "--buffer-bps") {
            o.buffer_bps = std::strtod(next(), nullptr);
        } else if (a == "--vol-mult") {
            o.vol_mult = std::strtod(next(), nullptr);
        } else if (a == "--gamma") {
            o.gamma = std::strtod(next(), nullptr);
        } else if (a == "--horizon-s") {
            o.horizon_s = std::strtod(next(), nullptr);
        } else if (a == "--latency-us") {
            o.latency_us = static_cast<uint64_t>(std::strtoull(next(), nullptr, 10));
        } else if (!a.starts_with("--")) {
            o.files.emplace_back(a);
        }
    }
    return o;
}

// Routes only the chosen symbol into the simulator; a recording holds several.
struct SymbolFilter {
    llt::Symbol         want;
    llt::strat::Simulator* sim = nullptr;

    void on_book_ticker(const llt::BookTicker& bt) {
        if (bt.symbol == want) sim->on_book_ticker(bt);
    }
    void on_depth(const llt::DepthSnapshot& d) {
        if (d.symbol == want) sim->on_depth(d);
    }
    void on_trade(const llt::Trade& t) {
        if (t.symbol == want) sim->on_trade(t);
    }
};

// Counts events per symbol so the tool can pick a default and report coverage.
struct SymbolCensus {
    std::map<std::string, uint64_t> events;
    std::map<std::string, double>   last_mid;

    void on_book_ticker(const llt::BookTicker& bt) {
        const std::string s = std::string(bt.symbol.view());
        ++events[s];
        last_mid[s] = bt.mid().to_double();
    }
    void on_depth(const llt::DepthSnapshot& d) { ++events[std::string(d.symbol.view())]; }
    void on_trade(const llt::Trade& t) { ++events[std::string(t.symbol.view())]; }
};

template <typename Handler>
bool replay_files(const std::vector<std::string>& files, Handler& handler) {
    llt::binance::FeedStats stats;
    for (const auto& path : files) {
        llt::md::RecordReader reader;
        if (!reader.open(path)) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return false;
        }
        llt::md::RecordHeader hdr;
        std::string_view      payload;
        while (reader.next(hdr, payload)) {
            static_cast<void>(
                llt::binance::dispatch_payload(payload, hdr.recv_tsc, stats, handler));
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    llt::clk::clock_init();
    Options opt = parse_args(argc, argv);

    if (opt.files.empty()) {
        std::fprintf(stderr, "usage: backtest FILE.mdlog [...] [--symbol SYM] [options]\n");
        return 1;
    }

    SymbolCensus census;
    if (!replay_files(opt.files, census)) return 1;
    if (census.events.empty()) {
        std::fprintf(stderr, "no decodable events in the given files\n");
        return 1;
    }

    if (opt.symbol.empty()) {
        opt.symbol = std::max_element(census.events.begin(), census.events.end(),
                                      [](const auto& a, const auto& b) {
                                          return a.second < b.second;
                                      })
                         ->first;
        std::printf("no --symbol given, using the busiest: %s\n", opt.symbol.c_str());
    }

    const auto found = census.events.find(opt.symbol);
    if (found == census.events.end()) {
        std::fprintf(stderr, "%s is not in this recording. present:\n", opt.symbol.c_str());
        for (const auto& [name, n] : census.events) {
            std::fprintf(stderr, "  %-14s %lu events\n", name.c_str(),
                         static_cast<unsigned long>(n));
        }
        return 1;
    }

    // Instrument rules come from the venue, not from guesswork: an order has to clear
    // both the notional floor and the lot-size floor.
    llt::binance::RestClient     rest;
    std::vector<llt::Instrument> instruments;
    llt::Instrument              inst;
    bool                         have_inst = false;

    if (rest.connect() && rest.fetch_instruments(instruments)) {
        for (const auto& i : instruments) {
            if (i.symbol.view() == opt.symbol) {
                inst      = i;
                have_inst = true;
                break;
            }
        }
        rest.close();
    }
    if (!have_inst) {
        std::fprintf(stderr, "could not fetch instrument rules for %s\n", opt.symbol.c_str());
        return 1;
    }

    const double mid = census.last_mid.count(opt.symbol) ? census.last_mid[opt.symbol] : 0.0;
    if (mid <= 0.0) {
        std::fprintf(stderr, "no quotes for %s in this recording\n", opt.symbol.c_str());
        return 1;
    }

    if (opt.qty <= 0.0) {
        const double min_notional =
            std::max(inst.min_notional.to_double(), inst.min_qty.to_double() * mid);
        opt.qty = min_notional / mid;
    }
    if (opt.max_pos <= 0.0) opt.max_pos = opt.qty * 4.0;

    llt::strat::SimConfig cfg;
    cfg.instrument              = inst;
    cfg.maker_fee_bps           = opt.fee_bps / 2.0;
    cfg.latency_ns              = opt.latency_us * 1000;
    cfg.quoter.order_qty        = round_up(llt::Fixed::from_double(opt.qty), inst.step_size);
    cfg.quoter.max_position     = llt::Fixed::from_double(opt.max_pos);
    cfg.quoter.fee_bps          = opt.fee_bps;
    cfg.quoter.edge_buffer_bps  = opt.buffer_bps;
    cfg.quoter.vol_multiplier   = opt.vol_mult;
    cfg.quoter.inventory_gamma  = opt.gamma;
    cfg.quoter.vol_horizon_s    = opt.horizon_s;

    llt::strat::Simulator sim(cfg);
    SymbolFilter          filter{llt::Symbol(opt.symbol), &sim};
    if (!replay_files(opt.files, filter)) return 1;

    const auto&  st  = sim.stats();
    const auto&  inv = sim.inventory();
    const double hours =
        static_cast<double>(st.total_ns) / 3.6e12;

    std::printf("\n%s over %.2f h  (%lu events)\n", opt.symbol.c_str(), hours,
                static_cast<unsigned long>(found->second));
    std::printf("quote size %.6g  max position %.6g  latency %lu us\n",
                cfg.quoter.order_qty.to_double(), opt.max_pos,
                static_cast<unsigned long>(opt.latency_us));
    std::printf("fees %.1f bps round trip, buffer %.1f, vol x%.2f, gamma %.2f\n\n",
                opt.fee_bps, opt.buffer_bps, opt.vol_mult, opt.gamma);

    std::printf("fills            %lu  (%lu bid / %lu ask)\n",
                static_cast<unsigned long>(st.fills),
                static_cast<unsigned long>(st.bid_fills),
                static_cast<unsigned long>(st.ask_fills));
    std::printf("requotes         %lu\n", static_cast<unsigned long>(st.requotes));
    std::printf("quoted           %.1f%% of the time\n",
                st.total_ns > 0 ? 100.0 * static_cast<double>(st.quoted_ns) /
                                      static_cast<double>(st.total_ns)
                                : 0.0);
    std::printf("max |position|   %.6g\n", st.max_abs_position.to_double());

    const double realized   = inv.realized_pnl().to_double();
    const double unrealized = inv.unrealized_pnl(sim.mark()).to_double();
    const double fees       = inv.fees_paid().to_double();

    std::printf("\nrealised        %+.6f\n", realized);
    std::printf("open            %+.6f\n", unrealized);
    std::printf("fees            %-.6f\n", fees);
    std::printf("net             %+.6f USDT\n", realized + unrealized - fees);

    std::printf("\nadverse selection (mid drift after a fill, + is in our favour)\n");
    if (st.adverse_100ms_n > 0) {
        std::printf("  +100ms  %+.3f bps over %lu fills\n",
                    st.adverse_100ms_bps / static_cast<double>(st.adverse_100ms_n),
                    static_cast<unsigned long>(st.adverse_100ms_n));
    }
    if (st.adverse_1s_n > 0) {
        std::printf("  +1s     %+.3f bps over %lu fills\n",
                    st.adverse_1s_bps / static_cast<double>(st.adverse_1s_n),
                    static_cast<unsigned long>(st.adverse_1s_n));
    }
    if (st.fills == 0) {
        std::printf("\nno fills. diagnosis:\n");
        std::printf("  trades reaching our price   %lu\n",
                    static_cast<unsigned long>(st.trades_reaching_quote));
        std::printf("  of those, blocked by queue  %lu\n",
                    static_cast<unsigned long>(st.blocked_by_queue));
        std::printf("  last queue ahead of us      %.6g\n", st.last_queue_ahead);
        std::printf("  quote suppressed by guards  %lu times\n",
                    static_cast<unsigned long>(st.quote_suppressed));
        if (st.trades_reaching_quote == 0) {
            std::printf("\n  nothing traded through our price: the spread we require is\n");
            std::printf("  wider than the market ever came to us.\n");
        }
    }

    return 0;
}
