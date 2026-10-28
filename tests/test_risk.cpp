#include "harness.hpp"
#include "llt/risk/deadman.hpp"
#include "llt/risk/kill_switch.hpp"
#include "llt/risk/limits.hpp"

#include <cstdio>
#include <string>
#include <unistd.h>

using llt::Fixed;
using llt::Side;
using llt::risk::DeadMan;
using llt::risk::HaltReason;
using llt::risk::KillSwitch;
using llt::risk::RiskConfig;
using llt::risk::RiskManager;
using llt::risk::Veto;

namespace {

Fixed f(double v) { return Fixed::from_double(v); }

constexpr uint64_t ms(uint64_t n) { return n * uint64_t{1'000'000}; }

RiskConfig small_account() {
    RiskConfig c;
    c.max_position           = f(100.0);
    c.max_notional           = f(50.0);
    c.max_order_notional     = f(10.0);
    c.max_loss               = f(2.0);
    c.max_price_deviation_bps = 200.0;
    c.stale_data_ms          = 2'000;
    c.halt_on_stale_ms       = 60'000;
    c.max_consecutive_rejects = 10;
    return c;
}

// A manager with data already flowing, so tests can focus on one limit at a time.
RiskManager live(RiskConfig c = small_account(), uint64_t at = ms(1'000)) {
    RiskManager r(c);
    r.on_market_data(at);
    return r;
}

}  // namespace

// ------------------------------------------------------------------- safe defaults

LLT_TEST(risk_default_config_permits_nothing) {
    // Zero is "deny", not "unlimited". A config nobody filled in must stop the engine.
    RiskManager r{RiskConfig{}};
    r.on_market_data(ms(1'000));
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(1.0), f(1.0), Fixed{}, ms(1'000)) !=
              Veto::None);
}

LLT_TEST(risk_passes_an_order_inside_every_limit) {
    auto r = live();
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(5.0), f(1.0), Fixed{}, ms(1'000)) ==
              Veto::None);
    LLT_CHECK(r.vetoes() == 0U);
}

// ----------------------------------------------------------------- exposure limits

LLT_TEST(risk_measures_the_position_the_order_would_create) {
    RiskConfig c      = small_account();
    c.max_position    = f(10.0);
    auto r            = live(c);

    // Already at the ceiling: holding 10 is fine, buying one more is not.
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(1.0), f(1.0), f(10.0), ms(1'000)) ==
              Veto::PositionLimit);
    // Selling reduces, so the same size in the other direction is allowed.
    LLT_CHECK(r.check_order(Side::Sell, f(1.0), f(1.0), f(1.0), f(10.0), ms(1'000)) ==
              Veto::None);
}

LLT_TEST(risk_limits_a_short_position_by_magnitude) {
    RiskConfig c   = small_account();
    c.max_position = f(10.0);
    auto r         = live(c);
    LLT_CHECK(r.check_order(Side::Sell, f(1.0), f(1.0), f(1.0), f(-10.0), ms(1'000)) ==
              Veto::PositionLimit);
}

LLT_TEST(risk_caps_a_single_order_notional) {
    auto r = live();  // max_order_notional 10
    LLT_CHECK(r.check_order(Side::Buy, f(2.0), f(6.0), f(2.0), Fixed{}, ms(1'000)) ==
              Veto::OrderTooLarge);
}

LLT_TEST(risk_caps_total_notional_even_when_quantity_fits) {
    RiskConfig c         = small_account();
    c.max_position       = f(1'000'000.0);  // quantity is not the binding limit
    c.max_notional       = f(20.0);
    c.max_order_notional = f(1'000.0);
    auto r               = live(c);
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(30.0), f(1.0), Fixed{}, ms(1'000)) ==
              Veto::NotionalLimit);
}

LLT_TEST(risk_rejects_a_price_far_from_the_reference) {
    auto r = live();  // band is 200 bps
    LLT_CHECK(r.check_order(Side::Buy, f(1.05), f(1.0), f(1.0), Fixed{}, ms(1'000)) ==
              Veto::PriceOutOfBand);
    LLT_CHECK(r.check_order(Side::Buy, f(1.01), f(1.0), f(1.0), Fixed{}, ms(1'000)) ==
              Veto::None);
}

LLT_TEST(risk_refuses_to_reason_about_a_zero_reference) {
    auto r = live();
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(1.0), Fixed{}, Fixed{}, ms(1'000)) ==
              Veto::BadReference);
}

// --------------------------------------------------------------------- stale data

LLT_TEST(risk_blocks_before_any_market_data_has_arrived) {
    RiskManager r{small_account()};
    LLT_CHECK(r.data_stale(ms(1'000)));
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(1.0), f(1.0), Fixed{}, ms(1'000)) ==
              Veto::NoMarketData);
}

LLT_TEST(risk_blocks_while_data_is_stale_and_recovers_when_it_returns) {
    auto r = live();  // data at t=1000ms, stale after 2000ms
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(1.0), f(1.0), Fixed{}, ms(4'000)) ==
              Veto::StaleData);
    r.on_market_data(ms(4'000));
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(1.0), f(1.0), Fixed{}, ms(4'000)) ==
              Veto::None);
}

LLT_TEST(risk_pulls_resting_orders_when_the_feed_goes_quiet) {
    // Quotes priced off a book we can no longer see must not be left out there.
    auto r = live();
    LLT_CHECK(!r.should_cancel_all(ms(1'500)));
    LLT_CHECK(r.should_cancel_all(ms(4'000)));
}

LLT_TEST(risk_halts_when_the_feed_stays_down) {
    auto r = live();
    r.poll(ms(30'000));
    LLT_CHECK(!r.halted());  // a gap is not yet a failure
    r.poll(ms(90'000));
    LLT_CHECK(r.halt_reason() == HaltReason::StaleDataTimeout);
}

// ------------------------------------------------------------------- the halt latch

LLT_TEST(risk_halts_on_the_loss_limit_counting_open_positions) {
    auto r = live();  // max_loss 2.0
    r.on_equity(f(-1.5));
    LLT_CHECK(!r.halted());
    r.on_equity(f(-2.0));
    LLT_CHECK(r.halt_reason() == HaltReason::MaxLoss);
}

LLT_TEST(risk_stays_halted_when_equity_recovers) {
    // The whole point of a latch: a bounce must not resume trading by itself.
    auto r = live();
    r.on_equity(f(-5.0));
    LLT_CHECK(r.halted());
    r.on_equity(f(+10.0));
    LLT_CHECK(r.halted());
    LLT_CHECK(r.halt_reason() == HaltReason::MaxLoss);
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(1.0), f(1.0), Fixed{}, ms(1'000)) ==
              Veto::Halted);
}

LLT_TEST(risk_keeps_the_first_halt_reason) {
    // The second failure is usually a consequence of the first; the diagnosis should
    // name the cause, not the symptom.
    auto r = live();
    r.on_equity(f(-5.0));
    r.halt(HaltReason::Manual);
    LLT_CHECK(r.halt_reason() == HaltReason::MaxLoss);
}

LLT_TEST(risk_clears_only_on_an_explicit_reset) {
    auto r = live();
    r.halt(HaltReason::Manual);
    LLT_CHECK(r.halted());
    r.reset();
    LLT_CHECK(!r.halted());
    LLT_CHECK(r.check_order(Side::Buy, f(1.0), f(5.0), f(1.0), Fixed{}, ms(1'000)) ==
              Veto::None);
}

LLT_TEST(risk_halts_on_a_reject_storm) {
    RiskConfig c              = small_account();
    c.max_consecutive_rejects = 3;
    auto r                    = live(c);
    r.on_reject();
    r.on_reject();
    LLT_CHECK(!r.halted());
    r.on_reject();
    LLT_CHECK(r.halt_reason() == HaltReason::RejectStorm);
}

LLT_TEST(risk_reject_counter_measures_a_run_not_a_total) {
    RiskConfig c              = small_account();
    c.max_consecutive_rejects = 3;
    auto r                    = live(c);
    for (int i = 0; i < 10; ++i) {
        r.on_reject();
        r.on_reject();
        r.on_accept();
    }
    LLT_CHECK(!r.halted());
    LLT_CHECK(r.consecutive_rejects() == 0U);
}

LLT_TEST(risk_remembers_the_worst_equity_it_saw) {
    auto r = live();
    r.on_equity(f(0.5));
    r.on_equity(f(-1.25));
    r.on_equity(f(0.75));
    LLT_CHECK(r.worst_equity() == f(-1.25));
}

LLT_TEST(risk_counts_what_it_refused) {
    auto r = live();
    LLT_CHECK(r.check_order(Side::Buy, f(2.0), f(6.0), f(2.0), Fixed{}, ms(1'000)) ==
              Veto::OrderTooLarge);
    LLT_CHECK(r.vetoes() == 1U);
    LLT_CHECK(r.last_veto() == Veto::OrderTooLarge);
}

// -------------------------------------------------------------------- dead-man switch

LLT_TEST(deadman_is_unprotected_until_it_is_first_armed) {
    const DeadMan d(10'000, 2'000);
    LLT_CHECK(d.due(0));
    LLT_CHECK(d.lapsed(0));
    LLT_CHECK(!d.armed());
}

LLT_TEST(deadman_refreshes_well_inside_the_countdown) {
    DeadMan d(10'000, 2'000);
    d.sent(ms(1'000));
    LLT_CHECK(!d.due(ms(2'000)));
    LLT_CHECK(d.due(ms(3'000)));
    LLT_CHECK(!d.lapsed(ms(3'000)));
}

LLT_TEST(deadman_survives_one_missed_refresh) {
    // The refresh is capped at a third of the countdown precisely so a single late
    // tick cannot flatten a healthy book.
    DeadMan d(9'000, 3'000);
    d.sent(ms(1'000));
    LLT_CHECK(!d.lapsed(ms(7'000)));  // two intervals missed, still protected
    LLT_CHECK(d.lapsed(ms(11'000)));
}

LLT_TEST(deadman_clamps_a_refresh_interval_that_is_too_slow) {
    // Asking to refresh as often as the countdown would make every hiccup fatal.
    const DeadMan d(9'000, 9'000);
    LLT_CHECK(d.refresh_ms() == 3'000);
}

LLT_TEST(deadman_counts_its_refreshes) {
    DeadMan d(10'000, 2'000);
    d.sent(ms(1'000));
    d.sent(ms(3'000));
    LLT_CHECK(d.refreshes() == 2U);
}

// ----------------------------------------------------------------------- kill switch

LLT_TEST(kill_switch_is_open_when_the_file_is_absent) {
    KillSwitch k("/tmp/llt-kill-switch-absent-xyz", 0);
    LLT_CHECK(!k.engaged(ms(1'000)));
}

LLT_TEST(kill_switch_engages_when_the_file_appears_and_never_lets_go) {
    const std::string path = "/tmp/llt-kill-switch-test";
    ::unlink(path.c_str());

    KillSwitch k(path, 0);
    LLT_CHECK(!k.engaged(ms(1'000)));

    std::FILE* fp = std::fopen(path.c_str(), "w");
    LLT_CHECK(fp != nullptr);
    if (fp != nullptr) std::fclose(fp);

    LLT_CHECK(k.engaged(ms(2'000)));

    // Removing the file must not silently resume trading.
    ::unlink(path.c_str());
    LLT_CHECK(k.engaged(ms(3'000)));
}

LLT_TEST(kill_switch_does_not_stat_on_every_tick) {
    KillSwitch k("/tmp/llt-kill-switch-absent-xyz", 250);
    for (int i = 0; i < 1'000; ++i) {  // one second of 1 ms ticks
        static_cast<void>(k.engaged(ms(1'000) + ms(static_cast<uint64_t>(i))));
    }
    LLT_CHECK(k.checks() == 4U);  // at 0 ms, then 250, 500 and 750
}
