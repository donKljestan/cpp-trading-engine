#include "harness.hpp"
#include "llt/exec/order_gateway.hpp"

#include <string>

using llt::Fixed;
using llt::Instrument;
using llt::Side;
using llt::Symbol;
using llt::exec::OrderGateway;
using llt::exec::OrderManager;
using llt::exec::OrderRef;
using llt::exec::OrderReject;
using llt::exec::RateLimiter;
using llt::exec::SendMode;
using llt::exec::WsApiClient;

namespace {

Fixed f(double v) { return Fixed::from_double(v); }

Instrument tac() {
    Instrument i;
    i.symbol       = Symbol("TACUSDT");
    i.tick_size    = f(0.0000001);
    i.step_size    = f(1.0);
    i.min_qty      = f(1.0);
    i.min_notional = f(5.0);
    i.tradable     = true;
    return i;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ------------------------------------------------------------------ validation

LLT_TEST(gateway_accepts_a_conforming_order) {
    LLT_CHECK(llt::exec::validate(tac(), f(0.0012), f(5000.0)) == OrderReject::None);
}

LLT_TEST(gateway_rejects_a_misaligned_price) {
    // Half a tick: the venue would refuse this, so we refuse it first.
    const Fixed off_tick = Fixed{f(0.0012).raw + 5};
    LLT_CHECK(llt::exec::validate(tac(), off_tick, f(5000.0)) ==
              OrderReject::NotTickAligned);
}

LLT_TEST(gateway_rejects_a_misaligned_quantity) {
    LLT_CHECK(llt::exec::validate(tac(), f(0.0012), f(100.5)) ==
              OrderReject::NotStepAligned);
}

LLT_TEST(gateway_rejects_below_the_notional_floor) {
    // 1000 x 0.0012 = 1.2 USDT, under the 5 USDT minimum.
    LLT_CHECK(llt::exec::validate(tac(), f(0.0012), f(1000.0)) ==
              OrderReject::BelowMinNotional);
}

LLT_TEST(gateway_rejects_nonsense) {
    LLT_CHECK(llt::exec::validate(tac(), f(0.0), f(5000.0)) ==
              OrderReject::NonPositivePrice);
    LLT_CHECK(llt::exec::validate(tac(), f(0.0012), f(0.0)) ==
              OrderReject::NonPositiveQty);
}

// --------------------------------------------------------------- request shape

LLT_TEST(place_request_is_post_only_by_default) {
    const std::string p =
        llt::exec::build_place_params(tac(), Side::Buy, f(0.0012), f(5000.0), "llt000_1");

    LLT_CHECK(contains(p, R"("symbol":"TACUSDT")"));
    LLT_CHECK(contains(p, R"("side":"BUY")"));
    LLT_CHECK(contains(p, R"("type":"LIMIT")"));
    // GTX makes the venue reject rather than let the order cross and charge taker fees.
    LLT_CHECK(contains(p, R"("timeInForce":"GTX")"));
    LLT_CHECK(contains(p, R"("newClientOrderId":"llt000_1")"));
    LLT_CHECK(contains(p, R"("timestamp":)"));
}

LLT_TEST(place_request_formats_prices_without_exponents) {
    const std::string p =
        llt::exec::build_place_params(tac(), Side::Sell, f(0.0000012), f(9000.0), "x");

    // A price rendered as 1.2e-06 would be rejected by the venue.
    LLT_CHECK(contains(p, R"("price":"0.0000012")"));
    LLT_CHECK(!contains(p, "e-"));
    LLT_CHECK(contains(p, R"("side":"SELL")"));
}

LLT_TEST(cancel_request_identifies_the_order_by_our_own_id) {
    const std::string p = llt::exec::build_cancel_params(tac(), "llt007_000000000042");
    LLT_CHECK(contains(p, R"("origClientOrderId":"llt007_000000000042")"));
    LLT_CHECK(contains(p, R"("symbol":"TACUSDT")"));
}

// -------------------------------------------------------------------- gateway

LLT_TEST(gateway_sends_nothing_in_dry_run) {
    WsApiClient  api;
    OrderManager om;
    RateLimiter  rl;
    OrderGateway gw(api, om, rl, SendMode::DryRun);

    OrderRef ref;
    LLT_CHECK(gw.place(tac(), Side::Buy, f(0.0012), f(5000.0), 1'000, ref) ==
              OrderReject::None);

    LLT_CHECK_EQ(gw.stats().placed, 0ULL);
    LLT_CHECK_EQ(gw.stats().suppressed, 1ULL);
    // The request was still built, so a dry run shows exactly what would have gone out.
    LLT_CHECK(contains(gw.last_request(), R"("symbol":"TACUSDT")"));
}

LLT_TEST(dry_run_does_not_leak_order_slots) {
    WsApiClient  api;
    OrderManager om;
    RateLimiter  rl;
    OrderGateway gw(api, om, rl, SendMode::DryRun);

    OrderRef ref;
    for (int i = 0; i < 200; ++i) {
        LLT_CHECK(gw.place(tac(), Side::Buy, f(0.0012), f(5000.0), 1'000, ref) ==
                  OrderReject::None);
    }
    LLT_CHECK_EQ(om.live(), 0U);
}

LLT_TEST(gateway_refuses_an_invalid_order_before_the_venue_can) {
    WsApiClient  api;
    OrderManager om;
    RateLimiter  rl;
    OrderGateway gw(api, om, rl, SendMode::DryRun);

    OrderRef ref;
    LLT_CHECK(gw.place(tac(), Side::Buy, f(0.0012), f(1000.0), 1'000, ref) ==
              OrderReject::BelowMinNotional);

    LLT_CHECK_EQ(gw.stats().rejected, 1ULL);
    LLT_CHECK_EQ(gw.stats().suppressed, 0ULL);
    // A rejected order must not consume a slot or a rate-limit token.
    LLT_CHECK_EQ(om.live(), 0U);
}

LLT_TEST(gateway_stops_at_the_rate_limit) {
    WsApiClient  api;
    OrderManager om;
    RateLimiter  rl;
    OrderGateway gw(api, om, rl, SendMode::DryRun);

    OrderRef ref;
    for (int i = 0; i < 300; ++i) {
        LLT_CHECK(gw.place(tac(), Side::Buy, f(0.0012), f(5000.0), 0, ref) ==
                  OrderReject::None);
    }
    LLT_CHECK(gw.place(tac(), Side::Buy, f(0.0012), f(5000.0), 0, ref) ==
              OrderReject::RateLimited);
}

LLT_TEST(live_mode_without_a_session_refuses_rather_than_pretending) {
    WsApiClient  api;  // never connected
    OrderManager om;
    RateLimiter  rl;
    OrderGateway gw(api, om, rl, SendMode::Live);

    OrderRef ref;
    LLT_CHECK(gw.place(tac(), Side::Buy, f(0.0012), f(5000.0), 1'000, ref) ==
              OrderReject::NotConnected);
    LLT_CHECK_EQ(gw.stats().placed, 0ULL);
    // The slot must not be left believing an order is live out there.
    LLT_CHECK_EQ(om.live(), 0U);
}
