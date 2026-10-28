#include "harness.hpp"
#include "llt/exec/order_manager.hpp"
#include "llt/venue/binance/bn_user.hpp"

#include <string>

using llt::Fixed;
using llt::Side;
using llt::binance::AccountUpdate;
using llt::binance::ExecReport;
using llt::exec::OrderState;

namespace {

// Shape taken from the venue's own documentation of ORDER_TRADE_UPDATE.
constexpr const char* kPartialFill = R"({
  "e":"ORDER_TRADE_UPDATE","E":1568879465651,"T":1568879465650,
  "o":{"s":"BTCUSDT","c":"llt007_000000000042","S":"SELL","o":"LIMIT","f":"GTX",
       "q":"0.500","p":"9910","ap":"9910","sp":"0","x":"TRADE","X":"PARTIALLY_FILLED",
       "i":8886774,"l":"0.200","z":"0.200","L":"9910","n":"0.00198","N":"USDT",
       "T":1568879465650,"t":42,"m":true,"R":false,"wt":"CONTRACT_PRICE"}})";

constexpr const char* kPostOnlyRejected = R"({
  "e":"ORDER_TRADE_UPDATE","E":1568879465651,"T":1568879465650,
  "o":{"s":"TACUSDT","c":"llt003_000000000007","S":"BUY","o":"LIMIT","f":"GTX",
       "q":"45","p":"0.0012","x":"EXPIRED","X":"EXPIRED","i":991,"l":"0","z":"0",
       "L":"0","T":1568879465650,"t":0,"m":false}})";

constexpr const char* kAccount = R"({
  "e":"ACCOUNT_UPDATE","E":1564745798939,"T":1564745798938,
  "a":{"m":"ORDER",
       "B":[{"a":"USDT","wb":"4998.12","cw":"4998.12","bc":"0"},
            {"a":"BNB","wb":"0.5","cw":"0.5","bc":"0"}],
       "P":[{"s":"TACUSDT","pa":"45","ep":"0.0012","up":"-0.0003","mt":"isolated"}]}})";

}  // namespace

LLT_TEST(user_parses_a_partial_fill) {
    ExecReport r;
    LLT_CHECK(llt::binance::parse_order_update(kPartialFill, r));

    LLT_CHECK(r.symbol.view() == "BTCUSDT");
    LLT_CHECK(std::string(r.client_id) == "llt007_000000000042");
    LLT_CHECK(r.side == Side::Sell);
    LLT_CHECK(r.state == OrderState::PartiallyFilled);
    LLT_CHECK_EQ(r.cum_filled.raw, Fixed::from_double(0.2).raw);
    LLT_CHECK_EQ(r.last_qty.raw, Fixed::from_double(0.2).raw);
    LLT_CHECK_EQ(r.last_px.raw, Fixed::from_double(9910.0).raw);
    LLT_CHECK_EQ(r.order_id, 8886774ULL);
    LLT_CHECK_EQ(r.transact_ms, 1568879465650ULL);
}

LLT_TEST(user_parses_a_post_only_rejection_as_expired) {
    ExecReport r;
    LLT_CHECK(llt::binance::parse_order_update(kPostOnlyRejected, r));

    // GTX refusing to cross is the protection doing its job, not a failure.
    LLT_CHECK(r.state == OrderState::Expired);
    LLT_CHECK(llt::exec::is_terminal(r.state));
    LLT_CHECK_EQ(r.cum_filled.raw, 0);
}

LLT_TEST(user_refuses_to_guess_an_unknown_status) {
    // NEW_ADL is the venue deleveraging someone, not our order moving.
    OrderState s = OrderState::New;
    LLT_CHECK(!llt::binance::order_state_from("NEW_ADL", s));
    LLT_CHECK(!llt::binance::order_state_from("", s));
    LLT_CHECK(!llt::binance::order_state_from("SOMETHING_NEW_IN_2027", s));
}

LLT_TEST(user_maps_every_status_we_expect) {
    OrderState s = OrderState::Idle;
    LLT_CHECK(llt::binance::order_state_from("NEW", s) && s == OrderState::New);
    LLT_CHECK(llt::binance::order_state_from("PARTIALLY_FILLED", s) &&
              s == OrderState::PartiallyFilled);
    LLT_CHECK(llt::binance::order_state_from("FILLED", s) && s == OrderState::Filled);
    LLT_CHECK(llt::binance::order_state_from("CANCELED", s) && s == OrderState::Canceled);
    LLT_CHECK(llt::binance::order_state_from("EXPIRED", s) && s == OrderState::Expired);
    LLT_CHECK(llt::binance::order_state_from("REJECTED", s) && s == OrderState::Rejected);
}

LLT_TEST(user_rejects_a_truncated_update) {
    ExecReport r;
    LLT_CHECK(!llt::binance::parse_order_update(R"({"e":"ORDER_TRADE_UPDATE"})", r));
    // Missing the side and status fields entirely.
    LLT_CHECK(!llt::binance::parse_order_update(
        R"({"e":"ORDER_TRADE_UPDATE","o":{"s":"X","c":"llt000_000000000001"}})", r));
}

LLT_TEST(user_parses_an_account_update) {
    AccountUpdate a;
    LLT_CHECK(llt::binance::parse_account_update(kAccount, a));

    LLT_CHECK_EQ(a.wallet_usdt.raw, Fixed::from_double(4998.12).raw);
    LLT_CHECK_EQ(static_cast<int>(a.n_positions), 1);
    LLT_CHECK(a.positions[0].symbol.view() == "TACUSDT");
    LLT_CHECK_EQ(a.positions[0].amount.raw, Fixed::from_double(45.0).raw);
    LLT_CHECK_EQ(a.positions[0].entry_price.raw, Fixed::from_double(0.0012).raw);
}

LLT_TEST(exec_report_drives_the_order_manager) {
    llt::exec::OrderManager om;
    llt::exec::OrderRef     ref;
    LLT_CHECK(om.reserve(Side::Sell, Fixed::from_double(9910.0),
                         Fixed::from_double(0.5), 1'000, ref));

    // Rewrite the fixture's client id to the one this manager actually issued.
    std::string json = kPartialFill;
    const size_t at  = json.find("llt007_000000000042");
    LLT_CHECK(at != std::string::npos);
    json.replace(at, std::strlen(ref.client_id), ref.client_id);

    ExecReport r;
    LLT_CHECK(llt::binance::parse_order_update(json, r));

    const auto delta =
        om.on_update(r.client_id, r.state, r.cum_filled, r.last_px, 2'000);

    LLT_CHECK(delta.occurred);
    LLT_CHECK(delta.side == Side::Sell);
    LLT_CHECK_EQ(delta.qty.raw, Fixed::from_double(0.2).raw);
    LLT_CHECK(om.at(ref.slot).state == OrderState::PartiallyFilled);
}
