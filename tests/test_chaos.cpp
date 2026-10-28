#include "harness.hpp"
#include "llt/exec/order_manager.hpp"
#include "llt/exec/rate_limit.hpp"
#include "llt/risk/deadman.hpp"
#include "llt/risk/kill_switch.hpp"
#include "llt/risk/limits.hpp"

#include <cstdint>

// What the engine does when its assumptions are violated rather than when they hold.
//
// Every protection in this system is computed from elapsed time, and elapsed time is
// computed from a clock that can step. The server runs chrony and the development
// machine has no real-time clock at all, so this is not a thought experiment.
//
// The rule these tests encode: when a check cannot establish that something is safe,
// it must report unsafe. A protection that reports "fine" because it could not do the
// arithmetic is worse than no protection, because it is believed.

using llt::Fixed;
using llt::Side;
using llt::exec::OrderManager;
using llt::exec::OrderRef;
using llt::exec::RateLimiter;
using llt::exec::TokenBucket;
using llt::risk::DeadMan;
using llt::risk::HaltReason;
using llt::risk::KillSwitch;
using llt::risk::RiskConfig;
using llt::risk::RiskManager;
using llt::risk::Veto;

namespace {

Fixed f(double v) { return Fixed::from_double(v); }

constexpr uint64_t ms(uint64_t n) { return n * uint64_t{1'000'000}; }

RiskConfig limits() {
    RiskConfig c;
    c.max_position        = f(100.0);
    c.max_notional        = f(500.0);
    c.max_order_notional  = f(50.0);
    c.max_loss            = f(5.0);
    c.stale_data_ms       = 2'000;
    c.halt_on_stale_ms    = 60'000;
    return c;
}

}  // namespace

// ------------------------------------------------------- the clock steps backwards

LLT_TEST(chaos_backwards_clock_makes_market_data_stale) {
    RiskManager r{limits()};
    r.on_market_data(ms(10'000));

    LLT_CHECK(!r.data_stale(ms(10'500)));
    // NTP stepped the clock back. Nothing can be aged, so nothing may be trusted.
    LLT_CHECK(r.data_stale(ms(9'000)));
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(1.0), f(1.0), Fixed{}, ms(9'000)) ==
              Veto::StaleData);
}

LLT_TEST(chaos_backwards_clock_pulls_resting_orders) {
    RiskManager r{limits()};
    r.on_market_data(ms(10'000));
    LLT_CHECK(r.should_cancel_all(ms(9'000)));
}

LLT_TEST(chaos_backwards_clock_makes_the_dead_man_switch_refresh) {
    DeadMan d(10'000, 2'000);
    d.sent(ms(10'000));
    LLT_CHECK(!d.due(ms(10'500)));
    // Refreshing early costs one request; skipping it risks the venue flattening us.
    LLT_CHECK(d.due(ms(9'000)));
}

LLT_TEST(chaos_backwards_clock_means_the_countdown_cannot_be_trusted) {
    DeadMan d(10'000, 2'000);
    d.sent(ms(10'000));
    LLT_CHECK(!d.lapsed(ms(12'000)));
    // We cannot show the venue's timer survived, so we must assume it did not.
    LLT_CHECK(d.lapsed(ms(9'000)));
}

LLT_TEST(chaos_backwards_clock_does_not_mint_rate_limit_tokens) {
    TokenBucket bucket(10.0, 1.0);
    for (int i = 0; i < 10; ++i) {
        LLT_CHECK(bucket.try_take(1.0, ms(1'000)));
    }
    LLT_CHECK(!bucket.try_take(1.0, ms(1'000)));
    // Rewinding the clock must not look like time passing.
    LLT_CHECK(!bucket.try_take(1.0, ms(500)));
    LLT_CHECK(!bucket.try_take(1.0, ms(0)));
}

LLT_TEST(chaos_backwards_clock_still_sees_the_kill_switch) {
    KillSwitch k("/tmp/llt-chaos-absent", 250);
    LLT_CHECK(!k.engaged(ms(10'000)));
    const uint64_t before = k.checks();
    static_cast<void>(k.engaged(ms(1'000)));
    LLT_CHECK(k.checks() > before);
}

// ---------------------------------------------------------- the clock jumps forward

LLT_TEST(chaos_forward_clock_jump_halts_on_a_feed_that_cannot_be_that_old) {
    RiskManager r{limits()};
    r.on_market_data(ms(10'000));
    r.poll(ms(10'000) + ms(3'600'000));  // an hour appears to pass in one tick
    LLT_CHECK(r.halt_reason() == HaltReason::StaleDataTimeout);
}

LLT_TEST(chaos_forward_clock_jump_does_not_make_the_dead_man_look_fresh) {
    DeadMan d(10'000, 2'000);
    d.sent(ms(10'000));
    LLT_CHECK(d.lapsed(ms(10'000) + ms(60'000)));
    LLT_CHECK(d.due(ms(10'000) + ms(60'000)));
}

// --------------------------------------------------------------- the process dies

LLT_TEST(chaos_a_restarted_engine_owns_nothing_until_it_reconciles) {
    // After kill -9 the slot pool is empty while the venue may still hold orders.
    OrderManager restarted;
    LLT_CHECK(restarted.live() == 0U);

    restarted.begin_reconcile();
    restarted.saw("llt000_000000000001");  // an id this process never issued
    static_cast<void>(restarted.end_reconcile());

    // Silence here would mean trading alongside orders we cannot cancel or account for.
    LLT_CHECK(restarted.unclaimed() > 0U);
}

LLT_TEST(chaos_a_restarted_engine_forgets_orders_the_venue_no_longer_reports) {
    OrderManager om;
    OrderRef     ref;
    LLT_CHECK(om.reserve(Side::Buy, f(100.0), f(1.0), ms(1'000), ref));
    om.on_update(ref.client_id, llt::exec::OrderState::New, Fixed{}, Fixed{}, ms(1'100));
    LLT_CHECK(om.live() == 1U);

    // The venue lists nothing: whatever we thought was resting is not.
    om.begin_reconcile();
    const size_t dropped = om.end_reconcile();
    LLT_CHECK(dropped == 1U);
    LLT_CHECK(om.live() == 0U);
}

// ------------------------------------------------------- the feed dies and returns

LLT_TEST(chaos_a_reconnect_does_not_clear_a_halt) {
    // Data coming back is not evidence that whatever stopped us has been understood.
    RiskManager r{limits()};
    r.on_market_data(ms(10'000));
    r.on_equity(f(-10.0));
    LLT_CHECK(r.halted());

    r.on_market_data(ms(11'000));
    LLT_CHECK(r.halted());
    LLT_CHECK(r.should_cancel_all(ms(11'000)));
}
