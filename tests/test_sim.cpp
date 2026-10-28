#include "harness.hpp"
#include "llt/strat/sim.hpp"

#include <cstdint>

using llt::BookTicker;
using llt::DepthSnapshot;
using llt::Fixed;
using llt::Instrument;
using llt::Side;
using llt::Symbol;
using llt::Trade;
using llt::strat::SimConfig;
using llt::strat::Simulator;
using llt::strat::SimStats;

namespace {

Fixed f(double v) { return Fixed::from_double(v); }

// Priced near 1.0 with a 0.001 tick, so one tick is ten basis points and a quote a few
// bps from the mid still lands on a level the depth snapshot actually contains.
Instrument test_symbol() {
    Instrument i;
    i.symbol       = Symbol("TESTUSDT");
    i.tick_size    = f(0.001);
    i.step_size    = f(1.0);
    i.min_qty      = f(1.0);
    i.min_notional = f(1.0);
    i.tradable     = true;
    return i;
}

SimConfig base_config() {
    SimConfig c;
    c.instrument          = test_symbol();
    c.maker_fee_bps       = 2.0;
    c.latency_ns          = 2'000'000;
    c.quoter.order_qty    = f(10.0);
    c.quoter.max_position = f(1'000.0);
    c.quoter.fee_bps      = 4.0;
    c.quoter.edge_buffer_bps = 0.0;  // half spread is just the fee, so we sit at the touch
    c.quoter.vol_multiplier  = 0.0;  // volatility must be positive but must not widen us
    c.quoter.inventory_gamma = 0.0;
    c.quoter.max_abs_obi     = 2.0;  // guards off: this file tests the fill model
    c.quoter.max_abs_tfi     = 2.0;
    c.quoter.min_requote_ticks = 1;
    c.quoter.min_requote_ms    = 0;
    return c;
}

BookTicker book(uint64_t t_ms, double bid, double ask) {
    BookTicker bt;
    bt.symbol           = Symbol("TESTUSDT");
    bt.time.exchange_ms = t_ms;
    bt.time.transact_ms = t_ms;
    bt.bid_px           = f(bid);
    bt.ask_px           = f(ask);
    bt.bid_qty          = f(100.0);
    bt.ask_qty          = f(100.0);
    return bt;
}

DepthSnapshot depth(uint64_t t_ms, double resting) {
    DepthSnapshot d;
    d.symbol           = Symbol("TESTUSDT");
    d.time.exchange_ms = t_ms;
    d.time.transact_ms = t_ms;
    d.n_bids           = 20;
    d.n_asks           = 20;
    for (size_t i = 0; i < DepthSnapshot::kMaxLevels; ++i) {
        d.bids[i].px  = f(1.000 - 0.001 * static_cast<double>(i));
        d.bids[i].qty = f(resting);
        d.asks[i].px  = f(1.001 + 0.001 * static_cast<double>(i));
        d.asks[i].qty = f(resting);
    }
    return d;
}

Trade trade(uint64_t t_ms, double px, double qty, Side aggressor) {
    Trade t;
    t.symbol           = Symbol("TESTUSDT");
    t.time.exchange_ms = t_ms;
    t.time.transact_ms = t_ms;
    t.px               = f(px);
    t.qty              = f(qty);
    t.buyer_is_maker   = (aggressor == Side::Sell);
    return t;
}

// The quoter refuses to price until it has a volatility estimate, so the mid has to
// move before anything rests. It then settles so the quote stops chasing.
void warm_up(Simulator& sim, uint64_t& t, double resting) {
    for (int i = 0; i < 14; ++i) {
        const bool up = (i % 2) == 0;
        sim.on_depth(depth(t, resting));
        sim.on_book_ticker(book(t, up ? 1.000 : 1.001, up ? 1.001 : 1.002));
        t += 100;
    }
    for (int i = 0; i < 6; ++i) {
        sim.on_depth(depth(t, resting));
        sim.on_book_ticker(book(t, 1.000, 1.001));
        t += 100;
    }
}

// Aggressive sells priced under every bid, so each one walks the book down to us.
SimStats sweep_the_bids(const SimConfig& cfg, double resting, double qty, int n) {
    Simulator sim(cfg);
    uint64_t  t = 1'000;
    warm_up(sim, t, resting);

    for (int i = 0; i < n; ++i) {
        sim.on_trade(trade(t, 0.5, qty, Side::Sell));
        t += 10;
    }
    return sim.stats();
}

}  // namespace

LLT_TEST(sim_quotes_once_it_has_seen_enough_of_the_market) {
    // If this fails, every other test in this file is measuring nothing.
    const SimStats st = sweep_the_bids(base_config(), 1.0, 1.0, 1);
    LLT_CHECK(st.requotes > 0);
    LLT_CHECK(st.quoted_ns > 0);
    LLT_CHECK(st.trades_reaching_quote > 0);
}

// ------------------------------------------------------------------ the queue model

LLT_TEST(sim_blocks_a_fill_behind_a_queue_larger_than_the_trade) {
    const SimStats st = sweep_the_bids(base_config(), 5'000.0, 10.0, 20);
    LLT_CHECK(st.trades_reaching_quote == 20);
    LLT_CHECK(st.fills == 0);
    LLT_CHECK(st.blocked_by_queue == 20);
}

LLT_TEST(sim_fills_when_a_trade_clears_the_queue_ahead) {
    const SimStats st = sweep_the_bids(base_config(), 10.0, 1'000.0, 1);
    LLT_CHECK(st.fills > 0);
    LLT_CHECK(st.bid_fills == st.fills);
    LLT_CHECK(st.ask_fills == 0);
}

LLT_TEST(sim_cancellation_ahead_of_us_turns_a_blocked_trade_into_a_fill) {
    SimConfig      cfg     = base_config();
    const SimStats without = sweep_the_bids(cfg, 1'000.0, 100.0, 8);
    cfg.cancel_per_trade   = 8.0;
    const SimStats with    = sweep_the_bids(cfg, 1'000.0, 100.0, 8);

    LLT_CHECK(without.fills == 0);
    LLT_CHECK(with.fills > 0);
}

LLT_TEST(sim_cancellation_never_reduces_the_fill_count) {
    // Monotonicity. Every conclusion drawn from the cancellation sweep assumes it.
    uint64_t previous = 0;
    for (const double k : {0.0, 2.0, 8.0, 32.0}) {
        SimConfig cfg        = base_config();
        cfg.cancel_per_trade = k;
        const SimStats st    = sweep_the_bids(cfg, 2'000.0, 50.0, 40);
        LLT_CHECK(st.fills >= previous);
        previous = st.fills;
    }
    LLT_CHECK(previous > 0);
}

LLT_TEST(sim_cancelled_size_does_not_trade_against_us) {
    // Cancellation clears the way. It must never be counted as volume we filled.
    SimConfig cfg        = base_config();
    cfg.cancel_per_trade = 1'000.0;

    Simulator sim(cfg);
    uint64_t  t = 1'000;
    warm_up(sim, t, 100.0);
    sim.on_trade(trade(t, 0.5, 1.0, Side::Sell));

    LLT_CHECK(sim.stats().volume <= f(1.0));
}

LLT_TEST(sim_ignores_trades_that_never_reach_our_price) {
    Simulator sim(base_config());
    uint64_t  t = 1'000;
    warm_up(sim, t, 100.0);

    for (int i = 0; i < 10; ++i) {
        sim.on_trade(trade(t, 1.0005, 1'000.0, Side::Sell));  // inside the spread
        t += 10;
    }
    LLT_CHECK(sim.stats().trades_reaching_quote == 0);
    LLT_CHECK(sim.stats().fills == 0);
}

// -------------------------------------------------------------------- accounting

LLT_TEST(sim_counts_every_fill_on_exactly_one_side) {
    const SimStats st = sweep_the_bids(base_config(), 1.0, 500.0, 10);
    LLT_CHECK(st.fills > 0);
    LLT_CHECK(st.bid_fills + st.ask_fills == st.fills);
}

LLT_TEST(sim_never_fills_more_often_than_trades_reach_it) {
    const SimStats st = sweep_the_bids(base_config(), 1.0, 500.0, 10);
    LLT_CHECK(st.fills <= st.trades_reaching_quote);
    LLT_CHECK(st.blocked_by_queue <= st.trades_reaching_quote);
}

LLT_TEST(sim_never_fills_more_than_the_order_it_placed) {
    // A single quote of 10 cannot trade more than 10, however large the print.
    const SimStats st = sweep_the_bids(base_config(), 1.0, 1'000'000.0, 1);
    LLT_CHECK(st.volume <= f(10.0));
}

LLT_TEST(sim_charges_the_maker_fee_on_traded_notional) {
    Simulator sim(base_config());
    uint64_t  t = 1'000;
    warm_up(sim, t, 1.0);
    for (int i = 0; i < 5; ++i) {
        sim.on_trade(trade(t, 0.5, 500.0, Side::Sell));
        sim.on_book_ticker(book(t, 1.000, 1.001));
        t += 100;
    }

    const double volume = sim.stats().volume.to_double();
    LLT_CHECK(volume > 0.0);

    // Everything filled near 1.0, so the fee should be about 2 bps of that notional.
    const double expected = volume * 1.0 * 2.0 / 1e4;
    const double fees     = sim.inventory().fees_paid().to_double();
    LLT_CHECK(fees > expected * 0.5);
    LLT_CHECK(fees < expected * 1.5);
}

LLT_TEST(sim_position_matches_the_fills_it_recorded) {
    Simulator sim(base_config());
    uint64_t  t = 1'000;
    warm_up(sim, t, 1.0);
    sim.on_trade(trade(t, 0.5, 500.0, Side::Sell));

    LLT_CHECK(sim.stats().bid_fills > 0);
    LLT_CHECK(sim.stats().ask_fills == 0);
    LLT_CHECK(sim.inventory().position() == sim.stats().volume);
}

// ------------------------------------------------------------------------ timing

LLT_TEST(sim_does_not_fill_before_the_quote_has_reached_the_venue) {
    SimConfig cfg  = base_config();
    cfg.latency_ns = 10'000'000'000;  // far beyond the horizon of this test

    const SimStats st = sweep_the_bids(cfg, 1.0, 10'000.0, 10);
    LLT_CHECK(st.fills == 0);
}

LLT_TEST(sim_quoted_time_never_exceeds_elapsed_time) {
    const SimStats st = sweep_the_bids(base_config(), 1'000.0, 10.0, 20);
    LLT_CHECK(st.total_ns > 0);
    LLT_CHECK(st.quoted_ns <= st.total_ns);
}

// -------------------------------------------------------------- adverse selection

LLT_TEST(sim_reports_adverse_selection_when_the_market_runs_from_a_buy) {
    Simulator sim(base_config());
    uint64_t  t = 1'000;
    warm_up(sim, t, 1.0);

    sim.on_trade(trade(t, 0.5, 500.0, Side::Sell));  // we are bought into
    LLT_CHECK(sim.stats().bid_fills > 0);
    t += 100;

    for (int i = 0; i < 40; ++i) {
        sim.on_book_ticker(book(t, 0.900, 0.901));
        t += 100;
    }

    LLT_CHECK(sim.stats().adverse_1s_n > 0);
    LLT_CHECK(sim.stats().adverse_1s_bps < 0.0);
}
