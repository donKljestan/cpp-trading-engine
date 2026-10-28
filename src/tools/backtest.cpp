// Replays a recording through the quoter and a fill simulator.
//
// The number that decides whether the strategy is viable is not the PnL line, which at
// this size is cents either way. It is adverse selection: if the mid consistently
// moves against a fill in the second after it happens, the quotes are being picked off
// and no parameter tuning fixes that.
//
//   backtest FILE.mdlog [...] [--symbol TACUSDT] [--sweep] [--sweep-queue]
//            [--qty 50] [--max-pos 200] [--fee-bps 4] [--buffer-bps 1]
//            [--vol-mult 1.0] [--gamma 1.0] [--horizon-s 5] [--latency-us 2000]
//            [--requote-ticks 1] [--requote-ms 100] [--queue-decay 0]
//            [--env production|demo]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "llt/core/clock.hpp"
#include "llt/md/record.hpp"
#include "llt/strat/sim.hpp"
#include "llt/venue/binance/bn_env.hpp"
#include "llt/venue/binance/bn_feed.hpp"
#include "llt/venue/binance/bn_rest.hpp"

namespace {

struct Options {
    std::string symbol;
    // Recordings are of the real market, so instrument rules must come from there too;
    // the demo book lists fewer symbols and would simply not have some of them.
    std::string env        = "production";
    bool        sweep      = false;
    bool        sweep_queue = false;
    double      qty        = 0.0;
    double      max_pos    = 0.0;
    double      fee_bps    = 4.0;
    double      buffer_bps = 1.0;
    double      vol_mult   = 1.0;
    double      gamma      = 1.0;
    double      horizon_s  = 5.0;
    uint64_t    latency_us = 2000;
    int64_t     requote_ticks = 1;
    uint64_t    requote_ms    = 100;
    double      queue_decay   = 0.0;
    double      cancel_per_trade = 0.0;

    std::vector<std::string> files;
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };

        if (a == "--symbol") {
            o.symbol = next();
        } else if (a == "--env") {
            o.env = next();
        } else if (a == "--sweep") {
            o.sweep = true;
        } else if (a == "--sweep-queue") {
            o.sweep_queue = true;
        } else if (a == "--queue-decay") {
            o.queue_decay = std::strtod(next(), nullptr);
        } else if (a == "--cancel-per-trade") {
            o.cancel_per_trade = std::strtod(next(), nullptr);
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
        } else if (a == "--requote-ticks") {
            o.requote_ticks = static_cast<int64_t>(std::strtoll(next(), nullptr, 10));
        } else if (a == "--requote-ms") {
            o.requote_ms = static_cast<uint64_t>(std::strtoull(next(), nullptr, 10));
        } else if (!a.starts_with("--")) {
            o.files.emplace_back(a);
        }
    }
    return o;
}

struct Frame {
    uint64_t    recv_tsc;
    std::string payload;
};

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

struct SymbolFilter {
    llt::Symbol            want;
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

// Keeps the symbol's frames in memory so a sweep replays from RAM instead of re-reading
// gigabytes once per parameter set.
bool load_frames(const std::vector<std::string>& files, std::string_view symbol,
                 std::vector<Frame>& out) {
    const std::string needle = "\"" + std::string(symbol) + "\"";

    for (const auto& path : files) {
        llt::md::RecordReader reader;
        if (!reader.open(path)) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return false;
        }
        llt::md::RecordHeader hdr;
        std::string_view      payload;
        while (reader.next(hdr, payload)) {
            if (payload.find(needle) == std::string_view::npos) continue;
            out.push_back({hdr.recv_tsc, std::string(payload)});
        }
    }
    return true;
}

struct Result {
    llt::strat::SimStats stats;
    double               realised   = 0.0;
    double               open       = 0.0;
    double               fees       = 0.0;
    double               net        = 0.0;
    double               adverse_1s = 0.0;
    // Gross edge per unit traded. This, not the PnL line, is what has to clear the
    // per-side fee: cents of PnL depend on size, bps of capture do not.
    double capture_bps = 0.0;
    double blocked_pct = 0.0;
};

Result run(const std::vector<Frame>& frames, const llt::strat::SimConfig& cfg,
           const llt::Symbol& symbol, double mid) {
    llt::strat::Simulator   sim(cfg);
    SymbolFilter            filter{symbol, &sim};
    llt::binance::FeedStats stats;

    for (const auto& f : frames) {
        static_cast<void>(
            llt::binance::dispatch_payload(f.payload, f.recv_tsc, stats, filter));
    }

    Result r;
    r.stats    = sim.stats();
    r.realised = sim.inventory().realized_pnl().to_double();
    r.open     = sim.inventory().unrealized_pnl(sim.mark()).to_double();
    r.fees     = sim.inventory().fees_paid().to_double();
    r.net      = r.realised + r.open - r.fees;
    if (r.stats.adverse_1s_n > 0) {
        r.adverse_1s =
            r.stats.adverse_1s_bps / static_cast<double>(r.stats.adverse_1s_n);
    }
    const double traded = r.stats.volume.to_double() * mid;
    if (traded > 0.0) r.capture_bps = (r.realised + r.open) / traded * 10'000.0;
    if (r.stats.trades_reaching_quote > 0) {
        r.blocked_pct = 100.0 * static_cast<double>(r.stats.blocked_by_queue) /
                        static_cast<double>(r.stats.trades_reaching_quote);
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    llt::clk::clock_init();
    Options opt = parse_args(argc, argv);

    if (opt.files.empty()) {
        std::fprintf(stderr, "usage: backtest FILE.mdlog [...] [--symbol SYM] [--sweep]\n");
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

    const auto& env = llt::binance::env_by_name(opt.env.c_str());

    llt::binance::RestClient     rest;
    std::vector<llt::Instrument> instruments;
    llt::Instrument              inst;
    bool                         have_inst = false;

    if (rest.connect(env.rest) && rest.fetch_instruments(instruments)) {
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
        std::fprintf(stderr, "no instrument rules for %s on %s (%s)\n", opt.symbol.c_str(),
                     env.name, env.rest);
        std::fprintf(stderr, "recordings are of the real market; try --env production\n");
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
    cfg.instrument             = inst;
    cfg.maker_fee_bps          = opt.fee_bps / 2.0;
    cfg.latency_ns             = opt.latency_us * 1000;
    cfg.quoter.order_qty       = round_up(llt::Fixed::from_double(opt.qty), inst.step_size);
    cfg.quoter.max_position    = llt::Fixed::from_double(opt.max_pos);
    cfg.quoter.fee_bps         = opt.fee_bps;
    cfg.quoter.edge_buffer_bps = opt.buffer_bps;
    cfg.quoter.vol_multiplier  = opt.vol_mult;
    cfg.quoter.inventory_gamma = opt.gamma;
    cfg.quoter.vol_horizon_s   = opt.horizon_s;
    cfg.quoter.min_requote_ticks = opt.requote_ticks;
    cfg.quoter.min_requote_ms    = opt.requote_ms;
    cfg.queue_decay_per_s        = opt.queue_decay;
    cfg.cancel_per_trade         = opt.cancel_per_trade;

    std::vector<Frame> frames;
    if (!load_frames(opt.files, opt.symbol, frames)) return 1;

    const llt::Symbol symbol(opt.symbol);

    if (opt.sweep_queue) {
        // `queue_probe` measures 4 to 12 units cancelled per unit traded, and that is
        // a lower bound. Zero is the old trades-only model, kept for comparison.
        const double fee_side = opt.fee_bps / 2.0;
        std::printf("\n%s  queue sensitivity over %zu frames\n", opt.symbol.c_str(),
                    frames.size());
        std::printf("capture must clear %.1f bps per fill to pay the fee\n", fee_side);
        std::printf("cxl/trd is units cancelled ahead of us per unit traded\n\n");
        std::printf("%8s %8s %8s %8s %10s %8s %9s\n", "cxl/trd", "buffer", "fills",
                    "cap_bps", "net USDT", "adv_1s", "blocked%");
        std::printf("%s\n", std::string(65, '-').c_str());

        for (const double k : {0.0, 4.0, 8.0, 12.0}) {
            for (const double buffer : {1.0, 2.0, 4.0}) {
                llt::strat::SimConfig c  = cfg;
                c.cancel_per_trade       = k;
                c.quoter.edge_buffer_bps = buffer;

                const Result r = run(frames, c, symbol, mid);
                std::printf("%8.1f %8.1f %8lu %8.2f %10.4f %8.2f %9.1f\n", k, buffer,
                            static_cast<unsigned long>(r.stats.fills), r.capture_bps,
                            r.net, r.adverse_1s, r.blocked_pct);
            }
        }
        std::printf("\nthe measured range is the middle of this table, not its edges.\n");
        return 0;
    }

    if (opt.sweep) {
        const double fee_side = opt.fee_bps / 2.0;
        std::printf("\n%s  sweep over %zu frames, quote size %.6g\n", opt.symbol.c_str(),
                    frames.size(), cfg.quoter.order_qty.to_double());
        std::printf("capture must clear %.1f bps per fill to pay the fee\n\n", fee_side);
        std::printf("%7s %5s %6s %9s %7s %8s %10s %8s %8s\n", "buffer", "vol", "ticks",
                    "requotes", "fills", "cap_bps", "net USDT", "adv_1s", "blocked%");
        std::printf("%s\n", std::string(79, '-').c_str());

        for (const double buffer : {1.0, 2.0, 4.0, 8.0}) {
            for (const double vm : {0.0, 2.0}) {
                // Every price change surrenders queue position, so how reluctant the
                // quoter is to move is a strategy parameter, not a detail.
                for (const int64_t ticks : {int64_t{1}, int64_t{8}, int64_t{64}}) {
                    llt::strat::SimConfig c   = cfg;
                    c.quoter.edge_buffer_bps  = buffer;
                    c.quoter.vol_multiplier   = vm;
                    c.quoter.min_requote_ticks = ticks;

                    const Result r = run(frames, c, symbol, mid);
                    std::printf("%7.1f %5.1f %6ld %9lu %7lu %8.2f %10.4f %8.2f %8.1f\n",
                                buffer, vm, static_cast<long>(ticks),
                                static_cast<unsigned long>(r.stats.requotes),
                                static_cast<unsigned long>(r.stats.fills), r.capture_bps,
                                r.net, r.adverse_1s, r.blocked_pct);
                }
            }
        }
        std::printf("\nwider quotes mean fewer fills; the question is whether what is\n");
        std::printf("captured per fill grows faster than the fill count falls.\n");
        return 0;
    }

    const Result r     = run(frames, cfg, symbol, mid);
    const auto&  st    = r.stats;
    const double hours = static_cast<double>(st.total_ns) / 3.6e12;

    std::printf("\n%s over %.2f h  (%lu events, rules from %s)\n", opt.symbol.c_str(),
                hours, static_cast<unsigned long>(found->second), env.name);
    std::printf("quote size %.6g  max position %.6g  latency %lu us\n",
                cfg.quoter.order_qty.to_double(), opt.max_pos,
                static_cast<unsigned long>(opt.latency_us));
    std::printf("fees %.1f bps round trip, buffer %.1f, vol x%.2f, gamma %.2f\n",
                opt.fee_bps, opt.buffer_bps, opt.vol_mult, opt.gamma);
    std::printf("requote after %ld tick(s) and %lu ms\n",
                static_cast<long>(opt.requote_ticks),
                static_cast<unsigned long>(opt.requote_ms));
    std::printf("queue decay %.2f/s, cancel per trade %.1f\n\n", opt.queue_decay,
                opt.cancel_per_trade);

    std::printf("fills            %lu  (%lu bid / %lu ask)\n",
                static_cast<unsigned long>(st.fills),
                static_cast<unsigned long>(st.bid_fills),
                static_cast<unsigned long>(st.ask_fills));
    std::printf("requotes         %lu  (%.2f/s)\n", static_cast<unsigned long>(st.requotes),
                hours > 0 ? static_cast<double>(st.requotes) / (hours * 3600.0) : 0.0);
    std::printf("quoted           %.1f%% of the time\n",
                st.total_ns > 0 ? 100.0 * static_cast<double>(st.quoted_ns) /
                                      static_cast<double>(st.total_ns)
                                : 0.0);
    std::printf("max |position|   %.6g\n", st.max_abs_position.to_double());

    std::printf("\nrealised        %+.6f\n", r.realised);
    std::printf("open            %+.6f\n", r.open);
    std::printf("fees            %-.6f\n", r.fees);
    std::printf("gross           %+.6f   (before fees)\n", r.realised + r.open);
    std::printf("net             %+.6f USDT\n", r.net);
    std::printf("capture         %+.2f bps per fill  (needs %.1f to pay the fee)\n",
                r.capture_bps, opt.fee_bps / 2.0);

    std::printf("\nadverse selection (mid drift after a fill, + is in our favour)\n");
    if (st.adverse_100ms_n > 0) {
        std::printf("  +100ms  %+.3f bps over %lu fills\n",
                    st.adverse_100ms_bps / static_cast<double>(st.adverse_100ms_n),
                    static_cast<unsigned long>(st.adverse_100ms_n));
    }
    if (st.adverse_1s_n > 0) {
        std::printf("  +1s     %+.3f bps over %lu fills\n", r.adverse_1s,
                    static_cast<unsigned long>(st.adverse_1s_n));
    }

    // Printed always, not only when there were no fills at all: a strategy that fills
    // only when a trade clears the whole queue ahead of it is being selected against,
    // and that shows up here long before it shows up in the PnL.
    std::printf("\nqueue\n");
    std::printf("  trades reaching our price   %lu\n",
                static_cast<unsigned long>(st.trades_reaching_quote));
    std::printf("  of those, blocked by queue  %lu  (%.1f%%)\n",
                static_cast<unsigned long>(st.blocked_by_queue), r.blocked_pct);
    std::printf("  last queue ahead of us      %.6g\n", st.last_queue_ahead);
    std::printf("  quote suppressed by guards  %lu times\n",
                static_cast<unsigned long>(st.quote_suppressed));
    if (st.fills > 0) {
        std::printf("  requotes per fill           %.1f\n",
                    static_cast<double>(st.requotes) / static_cast<double>(st.fills));
    }

    return 0;
}
