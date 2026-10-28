#include "harness.hpp"
#include "llt/strat/inventory.hpp"
#include "llt/strat/quoter.hpp"

using llt::Fixed;
using llt::Instrument;
using llt::Side;
using llt::Symbol;
using llt::strat::Inventory;
using llt::strat::Quote;
using llt::strat::Quoter;
using llt::strat::QuoterConfig;

namespace {

Fixed f(double v) { return Fixed::from_double(v); }

}  // namespace

// ---------------------------------------------------------------- inventory

LLT_TEST(inventory_starts_flat) {
    Inventory inv;
    LLT_CHECK(inv.flat());
    LLT_CHECK_EQ(inv.position().raw, 0);
    LLT_CHECK_EQ(inv.realized_pnl().raw, 0);
    LLT_CHECK_EQ(inv.unrealized_pnl(f(100.0)).raw, 0);
}

LLT_TEST(inventory_buy_then_sell_higher_is_a_profit) {
    Inventory inv;
    inv.on_fill(Side::Buy, f(100.0), f(2.0), Fixed{});
    LLT_CHECK_EQ(inv.position().raw, f(2.0).raw);
    LLT_CHECK_EQ(inv.avg_entry().raw, f(100.0).raw);

    inv.on_fill(Side::Sell, f(101.0), f(2.0), Fixed{});
    LLT_CHECK(inv.flat());
    LLT_CHECK_EQ(inv.realized_pnl().raw, f(2.0).raw);  // 1.00 x 2 units
}

LLT_TEST(inventory_short_then_cover_lower_is_a_profit) {
    Inventory inv;
    inv.on_fill(Side::Sell, f(100.0), f(3.0), Fixed{});
    LLT_CHECK_EQ(inv.position().raw, f(-3.0).raw);

    inv.on_fill(Side::Buy, f(99.0), f(3.0), Fixed{});
    LLT_CHECK(inv.flat());
    LLT_CHECK_EQ(inv.realized_pnl().raw, f(3.0).raw);
}

LLT_TEST(inventory_averages_entry_when_adding) {
    Inventory inv;
    inv.on_fill(Side::Buy, f(100.0), f(1.0), Fixed{});
    inv.on_fill(Side::Buy, f(102.0), f(3.0), Fixed{});

    LLT_CHECK_EQ(inv.position().raw, f(4.0).raw);
    LLT_CHECK_EQ(inv.avg_entry().raw, f(101.5).raw);  // (100 + 3x102) / 4
}

LLT_TEST(inventory_books_profit_on_a_partial_close) {
    Inventory inv;
    inv.on_fill(Side::Buy, f(100.0), f(4.0), Fixed{});
    inv.on_fill(Side::Sell, f(101.0), f(1.0), Fixed{});

    // A partially closed position must not read as flat-to-date.
    LLT_CHECK_EQ(inv.position().raw, f(3.0).raw);
    LLT_CHECK_EQ(inv.realized_pnl().raw, f(1.0).raw);
    LLT_CHECK_EQ(inv.avg_entry().raw, f(100.0).raw);  // unchanged by a reduction
}

LLT_TEST(inventory_handles_a_flip) {
    Inventory inv;
    inv.on_fill(Side::Buy, f(100.0), f(2.0), Fixed{});
    inv.on_fill(Side::Sell, f(101.0), f(5.0), Fixed{});

    LLT_CHECK_EQ(inv.position().raw, f(-3.0).raw);
    LLT_CHECK_EQ(inv.realized_pnl().raw, f(2.0).raw);   // closed 2 at +1.00
    LLT_CHECK_EQ(inv.avg_entry().raw, f(101.0).raw);    // the short opened here
}

LLT_TEST(inventory_unrealized_follows_the_mark) {
    Inventory inv;
    inv.on_fill(Side::Buy, f(100.0), f(2.0), Fixed{});

    LLT_CHECK_EQ(inv.unrealized_pnl(f(103.0)).raw, f(6.0).raw);
    LLT_CHECK_EQ(inv.unrealized_pnl(f(97.0)).raw, f(-6.0).raw);
}

LLT_TEST(inventory_equity_subtracts_fees) {
    Inventory inv;
    inv.on_fill(Side::Buy, f(100.0), f(1.0), f(0.02));
    inv.on_fill(Side::Sell, f(101.0), f(1.0), f(0.02));

    LLT_CHECK_EQ(inv.fees_paid().raw, f(0.04).raw);
    LLT_CHECK_EQ(inv.equity(f(101.0)).raw, f(0.96).raw);  // 1.00 realised - 0.04 fees
    LLT_CHECK_EQ(inv.fills(), 2ULL);
}

// ------------------------------------------------------------------- quoter

namespace {

Instrument test_instrument() {
    Instrument inst;
    inst.symbol       = Symbol("TESTUSDT");
    inst.tick_size    = f(0.01);
    inst.step_size    = f(0.001);
    inst.min_qty      = f(0.001);
    inst.min_notional = f(5.0);
    inst.tradable     = true;
    return inst;
}

QuoterConfig test_config() {
    QuoterConfig cfg;
    cfg.order_qty    = f(1.0);
    cfg.max_position = f(10.0);
    return cfg;
}

llt::strat::Signals signals_with(double bid, double bid_qty, double ask, double ask_qty,
                                 uint64_t ms) {
    llt::strat::Signals s;
    // Enough quotes for the volatility estimate to leave its warmup.
    for (uint64_t i = 1; i <= 15; ++i) {
        llt::BookTicker bt{};
        bt.symbol           = Symbol("TESTUSDT");
        bt.bid_px           = f(bid + (i % 2 ? 0.01 : 0.0));
        bt.bid_qty          = f(bid_qty);
        bt.ask_px           = f(ask + (i % 2 ? 0.01 : 0.0));
        bt.ask_qty          = f(ask_qty);
        bt.time.transact_ms = ms + i * 100;
        s.on_book_ticker(bt);
    }
    return s;
}

}  // namespace

LLT_TEST(quoter_half_spread_never_goes_below_cost) {
    const Quoter q(test_config());

    // Dead calm: the floor is fees plus the required buffer, not zero.
    LLT_CHECK(q.half_spread_bps(0.0) == 4.0 / 2.0 + 1.0);

    // Violent: risk takes over.
    LLT_CHECK(q.half_spread_bps(50.0) > 3.0);
}

LLT_TEST(quoter_is_silent_without_a_volatility_estimate) {
    Quoter    q(test_config());
    Inventory inv;

    llt::strat::Signals cold;
    llt::BookTicker     bt{};
    bt.symbol           = Symbol("TESTUSDT");
    bt.bid_px           = f(100.0);
    bt.bid_qty          = f(1.0);
    bt.ask_px           = f(100.1);
    bt.ask_qty          = f(1.0);
    bt.time.transact_ms = 1000;
    cold.on_book_ticker(bt);

    LLT_CHECK(!q.compute(cold, inv, test_instrument()).any());
}

LLT_TEST(quoter_straddles_fair_value_when_flat) {
    Quoter    q(test_config());
    Inventory inv;
    const auto sig = signals_with(100.0, 5.0, 100.1, 5.0, 1000);

    const Quote quote = q.compute(sig, inv, test_instrument());
    LLT_CHECK(quote.bid_active);
    LLT_CHECK(quote.ask_active);
    LLT_CHECK(quote.bid_px < quote.ask_px);
}

LLT_TEST(quoter_skews_down_when_long) {
    Quoter q(test_config());
    const auto sig = signals_with(100.0, 5.0, 100.1, 5.0, 1000);

    Inventory flat;
    Inventory lng;
    lng.on_fill(Side::Buy, f(100.0), f(8.0), Fixed{});

    const Quote a = q.compute(sig, flat, test_instrument());
    const Quote b = q.compute(sig, lng, test_instrument());

    // Holding a long, both sides move down: keener to sell, less keen to buy.
    LLT_CHECK(b.bid_px < a.bid_px);
    LLT_CHECK(b.ask_px <= a.ask_px);
}

LLT_TEST(quoter_skews_up_when_short) {
    Quoter q(test_config());
    const auto sig = signals_with(100.0, 5.0, 100.1, 5.0, 1000);

    Inventory flat;
    Inventory shrt;
    shrt.on_fill(Side::Sell, f(100.0), f(8.0), Fixed{});

    const Quote a = q.compute(sig, flat, test_instrument());
    const Quote b = q.compute(sig, shrt, test_instrument());

    LLT_CHECK(b.ask_px > a.ask_px);
    LLT_CHECK(b.bid_px >= a.bid_px);
}

LLT_TEST(quoter_stops_adding_at_the_position_limit) {
    Quoter q(test_config());
    const auto sig = signals_with(100.0, 5.0, 100.1, 5.0, 1000);

    Inventory maxed;
    maxed.on_fill(Side::Buy, f(100.0), f(10.0), Fixed{});

    const Quote quote = q.compute(sig, maxed, test_instrument());
    LLT_CHECK(!quote.bid_active);
    LLT_CHECK(quote.ask_active);
}

LLT_TEST(quoter_quotes_are_tick_aligned) {
    Quoter    q(test_config());
    Inventory inv;
    const auto sig  = signals_with(100.0, 5.0, 100.1, 5.0, 1000);
    const auto inst = test_instrument();

    const Quote quote = q.compute(sig, inv, inst);
    LLT_CHECK_EQ(quote.bid_px.raw % inst.tick_size.raw, 0);
    LLT_CHECK_EQ(quote.ask_px.raw % inst.tick_size.raw, 0);
}

LLT_TEST(quoter_never_crosses_the_touch) {
    QuoterConfig cfg = test_config();
    // A spread requirement far wider than the market's, which would otherwise push the
    // quotes through each other.
    cfg.edge_buffer_bps = 0.0;
    cfg.fee_bps         = 0.0;
    cfg.vol_multiplier  = 0.0;

    Quoter     q(cfg);
    Inventory  inv;
    const auto sig  = signals_with(100.0, 5.0, 100.1, 5.0, 1000);
    const auto inst = test_instrument();

    const Quote quote = q.compute(sig, inv, inst);
    LLT_CHECK(quote.bid_px < sig.book().ask_px);
    LLT_CHECK(quote.ask_px > sig.book().bid_px);
}

LLT_TEST(quoter_holds_still_for_a_small_move) {
    const Quoter q(test_config());
    const Fixed  tick = f(0.01);

    Quote live;
    live.bid_active = live.ask_active = true;
    live.bid_px = f(100.00);
    live.ask_px = f(100.10);

    Quote same = live;
    LLT_CHECK(!q.should_replace(live, same, tick, 10'000'000'000, 0));

    Quote moved = live;
    moved.bid_px = f(100.05);
    LLT_CHECK(q.should_replace(live, moved, tick, 10'000'000'000, 0));
}

LLT_TEST(quoter_respects_the_minimum_requote_interval) {
    const Quoter q(test_config());
    const Fixed  tick = f(0.01);

    Quote live;
    live.bid_active = live.ask_active = true;
    live.bid_px = f(100.00);
    live.ask_px = f(100.10);

    Quote moved  = live;
    moved.bid_px = f(100.05);

    // Same large move, 10 ms after the last change rather than 10 s.
    LLT_CHECK(!q.should_replace(live, moved, tick, 10'000'000, 0));
}

LLT_TEST(quoter_cancels_a_side_rather_than_standing_in_front_of_flow) {
    Quoter q(test_config());

    llt::strat::Signals sig = signals_with(100.0, 5.0, 100.1, 5.0, 1000);

    // Heavy, persistent buying: do not sit on the offer in front of it.
    for (uint64_t i = 1; i <= 20; ++i) {
        llt::Trade t{};
        t.symbol           = Symbol("TESTUSDT");
        t.px               = f(100.05);
        t.qty              = f(1.0);
        t.buyer_is_maker   = false;
        t.time.transact_ms = 3000 + i * 10;
        sig.on_trade(t);
    }
    LLT_CHECK(sig.tfi() > 0.75);

    Inventory   inv;
    const Quote quote = q.compute(sig, inv, test_instrument());
    LLT_CHECK(!quote.ask_active);
    LLT_CHECK(quote.bid_active);
}
