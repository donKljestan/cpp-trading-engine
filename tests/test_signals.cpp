#include "harness.hpp"
#include "llt/strat/signals.hpp"

#include <cmath>
#include <cstring>

using llt::BookTicker;
using llt::DepthSnapshot;
using llt::Fixed;
using llt::Side;
using llt::Symbol;
using llt::Trade;
using llt::strat::Signals;
using llt::strat::TimeEwma;

namespace {

constexpr uint64_t kSec = 1'000;  // one second, in the millisecond fields

BookTicker quote(double bid, double bid_qty, double ask, double ask_qty, uint64_t ms) {
    BookTicker bt{};
    bt.symbol           = Symbol("TESTUSDT");
    bt.bid_px           = Fixed::from_double(bid);
    bt.bid_qty          = Fixed::from_double(bid_qty);
    bt.ask_px           = Fixed::from_double(ask);
    bt.ask_qty          = Fixed::from_double(ask_qty);
    bt.time.transact_ms = ms;
    return bt;
}

Trade print(double px, double qty, bool buyer_is_maker, uint64_t ms) {
    Trade t{};
    t.symbol         = Symbol("TESTUSDT");
    t.px             = Fixed::from_double(px);
    t.qty            = Fixed::from_double(qty);
    t.buyer_is_maker = buyer_is_maker;
    t.time.transact_ms = ms;
    return t;
}

}  // namespace

LLT_TEST(ewma_first_update_takes_the_value) {
    TimeEwma e(10.0);
    LLT_CHECK(!e.ready());
    e.update(5.0, 1'000'000'000);
    LLT_CHECK(e.ready());
    LLT_CHECK(e.value() == 5.0);
}

LLT_TEST(ewma_half_life_is_honoured) {
    TimeEwma e(1.0);
    e.update(0.0, 0);
    e.update(1.0, 1'000'000'000);  // exactly one half-life later

    // After one half-life the average should sit halfway to the new value.
    LLT_CHECK(e.value() > 0.49);
    LLT_CHECK(e.value() < 0.51);
}

LLT_TEST(ewma_decays_by_elapsed_time_not_update_count) {
    TimeEwma fast(1.0);
    TimeEwma slow(1.0);

    fast.update(0.0, 0);
    slow.update(0.0, 0);

    // Twenty updates crammed into a millisecond must move the average far less than
    // one update a half-life later.
    for (int i = 1; i <= 20; ++i) fast.update(1.0, static_cast<uint64_t>(i) * 50'000);
    slow.update(1.0, 1'000'000'000);

    LLT_CHECK(fast.value() < slow.value());
    LLT_CHECK(fast.value() < 0.05);
}

LLT_TEST(signals_not_ready_before_first_quote) {
    Signals s;
    LLT_CHECK(!s.ready());
    LLT_CHECK(s.obi() == 0.0);
    LLT_CHECK(s.tfi() == 0.0);
    LLT_CHECK(s.vol_bps() == 0.0);
}

LLT_TEST(signals_fair_value_leans_away_from_the_heavy_side) {
    Signals s;
    // Far more size offered than bid: the next move is more likely down.
    s.on_book_ticker(quote(100.0, 1.0, 101.0, 9.0, kSec));

    LLT_CHECK(s.ready());
    LLT_CHECK(s.fair_value() < s.mid());
    LLT_CHECK(s.fair_value() > s.book().bid_px);
}

LLT_TEST(signals_trade_flow_sign_follows_the_aggressor) {
    Signals s;
    s.on_book_ticker(quote(100.0, 5.0, 101.0, 5.0, kSec));

    // buyer_is_maker == false means the buyer crossed the spread.
    for (uint64_t i = 1; i <= 10; ++i) s.on_trade(print(100.5, 1.0, false, kSec + i * 100));
    LLT_CHECK(s.tfi() > 0.9);

    Signals sells;
    sells.on_book_ticker(quote(100.0, 5.0, 101.0, 5.0, kSec));
    for (uint64_t i = 1; i <= 10; ++i) {
        sells.on_trade(print(100.5, 1.0, true, kSec + i * 100));
    }
    LLT_CHECK(sells.tfi() < -0.9);
}

LLT_TEST(signals_trade_flow_cancels_when_balanced) {
    Signals s;
    s.on_book_ticker(quote(100.0, 5.0, 101.0, 5.0, kSec));

    for (uint64_t i = 1; i <= 20; ++i) {
        s.on_trade(print(100.5, 1.0, i % 2 == 0, kSec + i * 10));
    }
    LLT_CHECK(std::fabs(s.tfi()) < 0.3);
}

LLT_TEST(signals_flow_stays_neutral_until_there_is_evidence) {
    Signals s;
    s.on_book_ticker(quote(100.0, 5.0, 101.0, 5.0, kSec));

    // A single print is literally 100% one-directional, but reporting that as a
    // maximal signal would have the quoter react to one trade.
    s.on_trade(print(100.5, 1.0, false, kSec + 10));
    LLT_CHECK(s.tfi() == 0.0);

    for (uint64_t i = 2; i <= 6; ++i) s.on_trade(print(100.5, 1.0, false, kSec + i * 10));
    LLT_CHECK(s.tfi() > 0.9);
}

LLT_TEST(signals_flow_decays_once_trading_stops) {
    Signals s;
    s.on_book_ticker(quote(100.0, 5.0, 101.0, 5.0, kSec));

    for (uint64_t i = 1; i <= 10; ++i) s.on_trade(print(100.5, 1.0, false, kSec + i * 10));
    LLT_CHECK(s.effective_trades() > 9.0);

    // Four half-lives of silence, then one small print to advance the clock.
    s.on_trade(print(100.5, 0.001, false, kSec + 20'000));
    LLT_CHECK(s.effective_trades() < 2.0);
}

LLT_TEST(signals_volatility_rises_with_price_movement) {
    Signals calm;
    Signals choppy;

    for (uint64_t i = 1; i <= 50; ++i) {
        const uint64_t ms = kSec + i * 100;
        calm.on_book_ticker(quote(100.0, 1.0, 100.1, 1.0, ms));
        const double shift = (i % 2 == 0) ? 0.5 : -0.5;
        choppy.on_book_ticker(quote(100.0 + shift, 1.0, 100.1 + shift, 1.0, ms));
    }

    LLT_CHECK(calm.vol_bps() < 0.001);
    LLT_CHECK(choppy.vol_bps() > 1.0);
}

LLT_TEST(signals_volatility_is_a_rate_not_a_per_update_figure) {
    // The same price path sampled twice as often must not report twice the volatility.
    Signals dense;
    Signals sparse;

    for (uint64_t i = 1; i <= 40; ++i) {
        const double shift = (i % 2 == 0) ? 0.2 : -0.2;
        dense.on_book_ticker(quote(100.0 + shift, 1.0, 100.1 + shift, 1.0, kSec + i * 100));
    }
    for (uint64_t i = 1; i <= 40; ++i) {
        const double shift = (i % 2 == 0) ? 0.2 : -0.2;
        sparse.on_book_ticker(quote(100.0 + shift, 1.0, 100.1 + shift, 1.0, kSec + i * 200));
    }

    // Sampling twice as fast doubles the number of moves per second, so the rate is
    // genuinely higher - but nowhere near the 2x a per-update average would show.
    const double ratio = dense.vol_bps() / sparse.vol_bps();
    LLT_CHECK(ratio > 1.0);
    LLT_CHECK(ratio < 1.6);
}

LLT_TEST(signals_obi_comes_from_depth) {
    Signals       s;
    DepthSnapshot d{};
    d.symbol = Symbol("TESTUSDT");
    d.n_bids = 2;
    d.n_asks = 2;
    d.bids[0] = {Fixed::from_double(100.0), Fixed::from_double(9.0)};
    d.bids[1] = {Fixed::from_double(99.9), Fixed::from_double(9.0)};
    d.asks[0] = {Fixed::from_double(100.1), Fixed::from_double(1.0)};
    d.asks[1] = {Fixed::from_double(100.2), Fixed::from_double(1.0)};

    s.on_depth(d);
    LLT_CHECK(s.obi() > 0.7);
}

LLT_TEST(signals_are_deterministic_for_the_same_input) {
    const auto run = [] {
        Signals s;
        for (uint64_t i = 1; i <= 100; ++i) {
            const double shift = static_cast<double>((i * 37) % 11) * 0.01;
            s.on_book_ticker(quote(100.0 + shift, 2.0, 100.1 + shift, 3.0, kSec + i * 50));
            s.on_trade(print(100.05 + shift, 0.5, i % 3 == 0, kSec + i * 50));
        }
        return s;
    };

    const Signals a = run();
    const Signals b = run();

    LLT_CHECK(a.vol_bps() == b.vol_bps());
    LLT_CHECK(a.tfi() == b.tfi());
    LLT_CHECK_EQ(a.fair_value().raw, b.fair_value().raw);
}
