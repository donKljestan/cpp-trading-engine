#include "harness.hpp"
#include "llt/exec/order_manager.hpp"
#include "llt/exec/rate_limit.hpp"

#include <string>

using llt::Fixed;
using llt::Side;
using llt::exec::OrderManager;
using llt::exec::OrderRef;
using llt::exec::OrderState;
using llt::exec::RateLimiter;
using llt::exec::TokenBucket;

namespace {

Fixed f(double v) { return Fixed::from_double(v); }

OrderRef place(OrderManager& om, Side side = Side::Buy, double px = 100.0,
               double qty = 2.0) {
    OrderRef ref;
    const bool ok = om.reserve(side, f(px), f(qty), 1'000, ref);
    LLT_CHECK(ok);
    return ref;
}

}  // namespace

// ------------------------------------------------------------- client order id

LLT_TEST(client_id_round_trips) {
    OrderManager om;
    const OrderRef ref = place(om);

    size_t   slot = 999;
    uint64_t seq  = 0;
    LLT_CHECK(llt::exec::parse_client_id(ref.client_id, slot, seq));
    LLT_CHECK_EQ(slot, ref.slot);
    LLT_CHECK_EQ(seq, ref.seq);
    LLT_CHECK_EQ(std::string(ref.client_id).size(), llt::exec::kClientIdLen);
}

LLT_TEST(client_id_rejects_foreign_ids) {
    size_t   slot = 0;
    uint64_t seq  = 0;
    LLT_CHECK(!llt::exec::parse_client_id("", slot, seq));
    LLT_CHECK(!llt::exec::parse_client_id("web_order_12345", slot, seq));
    LLT_CHECK(!llt::exec::parse_client_id("llt00X_000000000001", slot, seq));
    LLT_CHECK(!llt::exec::parse_client_id("xxx000_000000000001", slot, seq));
}

// ------------------------------------------------------------- state machine

LLT_TEST(order_lifecycle_new_then_filled) {
    OrderManager om;
    const OrderRef ref = place(om);

    LLT_CHECK(om.at(ref.slot).state == OrderState::PendingNew);
    LLT_CHECK_EQ(om.live(), 1U);

    static_cast<void>(om.on_update(ref.client_id, OrderState::New, Fixed{}, Fixed{}, 2'000));
    LLT_CHECK(om.at(ref.slot).state == OrderState::New);

    const auto delta =
        om.on_update(ref.client_id, OrderState::Filled, f(2.0), f(100.0), 3'000);
    LLT_CHECK(delta.occurred);
    LLT_CHECK_EQ(delta.qty.raw, f(2.0).raw);
    LLT_CHECK_EQ(om.live(), 0U);
}

LLT_TEST(order_reports_only_the_newly_executed_amount) {
    OrderManager om;
    const OrderRef ref = place(om, Side::Buy, 100.0, 10.0);

    // The venue sends cumulative quantities, so the delta is ours to compute.
    auto a = om.on_update(ref.client_id, OrderState::PartiallyFilled, f(3.0), f(100.0), 2'000);
    LLT_CHECK_EQ(a.qty.raw, f(3.0).raw);

    auto b = om.on_update(ref.client_id, OrderState::PartiallyFilled, f(7.0), f(100.0), 3'000);
    LLT_CHECK_EQ(b.qty.raw, f(4.0).raw);

    auto c = om.on_update(ref.client_id, OrderState::Filled, f(10.0), f(100.0), 4'000);
    LLT_CHECK_EQ(c.qty.raw, f(3.0).raw);
}

LLT_TEST(order_ignores_a_duplicated_update) {
    OrderManager om;
    const OrderRef ref = place(om, Side::Buy, 100.0, 10.0);

    auto first  = om.on_update(ref.client_id, OrderState::PartiallyFilled, f(4.0), f(100.0), 2'000);
    auto repeat = om.on_update(ref.client_id, OrderState::PartiallyFilled, f(4.0), f(100.0), 2'000);

    LLT_CHECK(first.occurred);
    LLT_CHECK(!repeat.occurred);
}

LLT_TEST(order_survives_an_update_arriving_before_the_acknowledgement) {
    OrderManager om;
    const OrderRef ref = place(om, Side::Buy, 100.0, 5.0);

    // The user data stream beat the order.place reply.
    auto fill = om.on_update(ref.client_id, OrderState::PartiallyFilled, f(2.0), f(100.0), 2'000);
    LLT_CHECK(fill.occurred);
    LLT_CHECK(om.at(ref.slot).state == OrderState::PartiallyFilled);

    // The late acknowledgement must not undo what already happened.
    static_cast<void>(om.on_update(ref.client_id, OrderState::New, Fixed{}, Fixed{}, 3'000));
    LLT_CHECK(om.at(ref.slot).state == OrderState::PartiallyFilled);
    LLT_CHECK_EQ(om.at(ref.slot).cum_filled.raw, f(2.0).raw);
}

LLT_TEST(order_ignores_everything_after_a_terminal_state) {
    OrderManager om;
    const OrderRef ref = place(om, Side::Buy, 100.0, 5.0);

    static_cast<void>(om.on_update(ref.client_id, OrderState::Canceled, Fixed{}, Fixed{}, 2'000));

    const auto late =
        om.on_update(ref.client_id, OrderState::Filled, f(5.0), f(100.0), 3'000);
    LLT_CHECK(!late.occurred);
}

LLT_TEST(post_only_rejection_is_expired_not_an_error) {
    OrderManager om;
    const OrderRef ref = place(om);

    // A GTX order that would have crossed. This is the protection working.
    static_cast<void>(om.on_update(ref.client_id, OrderState::Expired, Fixed{}, Fixed{}, 2'000));
    LLT_CHECK(llt::exec::is_terminal(OrderState::Expired));
    LLT_CHECK_EQ(om.live(), 0U);
}

LLT_TEST(cancel_request_is_separate_from_state) {
    OrderManager om;
    const OrderRef ref = place(om, Side::Buy, 100.0, 5.0);
    static_cast<void>(om.on_update(ref.client_id, OrderState::New, Fixed{}, Fixed{}, 2'000));

    om.request_cancel(ref.slot);
    LLT_CHECK(om.at(ref.slot).cancel_requested);

    // A fill can still land after the cancel is sent, and must be booked.
    const auto delta =
        om.on_update(ref.client_id, OrderState::PartiallyFilled, f(1.0), f(100.0), 3'000);
    LLT_CHECK(delta.occurred);
    LLT_CHECK(om.at(ref.slot).state == OrderState::PartiallyFilled);
}

// ------------------------------------------------------------------ slot pool

LLT_TEST(slot_is_reused_but_stale_updates_are_not_applied) {
    OrderManager om;
    const OrderRef first = place(om);
    static_cast<void>(om.on_update(first.client_id, OrderState::Canceled, Fixed{}, Fixed{}, 2'000));

    const OrderRef second = place(om);
    LLT_CHECK_EQ(second.slot, first.slot);  // the pool handed back the same slot
    LLT_CHECK(second.seq != first.seq);

    // An update for the dead order must not touch the live one.
    const auto ghost =
        om.on_update(first.client_id, OrderState::Filled, f(2.0), f(100.0), 3'000);
    LLT_CHECK(!ghost.occurred);
    LLT_CHECK_EQ(om.stale_updates(), 1ULL);
    LLT_CHECK(om.at(second.slot).state == OrderState::PendingNew);
}

LLT_TEST(updates_for_unknown_orders_are_counted_not_guessed_at) {
    OrderManager om;
    const auto delta =
        om.on_update("web_manual_order", OrderState::Filled, f(1.0), f(100.0), 2'000);

    LLT_CHECK(!delta.occurred);
    LLT_CHECK_EQ(om.unknown_updates(), 1ULL);
}

LLT_TEST(pool_refuses_to_overflow) {
    OrderManager om;
    OrderRef     ref;
    for (size_t i = 0; i < llt::exec::kMaxOrders; ++i) {
        LLT_CHECK(om.reserve(Side::Buy, f(100.0), f(1.0), 1'000, ref));
    }
    LLT_CHECK_EQ(om.live(), llt::exec::kMaxOrders);
    LLT_CHECK(!om.reserve(Side::Buy, f(100.0), f(1.0), 1'000, ref));
}

// -------------------------------------------------------------- reconciliation

LLT_TEST(reconcile_drops_orders_the_exchange_does_not_know_about) {
    OrderManager om;
    const OrderRef kept = place(om);
    const OrderRef lost = place(om);

    static_cast<void>(om.on_update(kept.client_id, OrderState::New, Fixed{}, Fixed{}, 2'000));
    static_cast<void>(om.on_update(lost.client_id, OrderState::New, Fixed{}, Fixed{}, 2'000));
    LLT_CHECK_EQ(om.live(), 2U);

    om.begin_reconcile();
    om.saw(kept.client_id);
    LLT_CHECK_EQ(om.end_reconcile(), 1U);

    LLT_CHECK_EQ(om.live(), 1U);
    LLT_CHECK_EQ(om.orphaned(), 1ULL);
    LLT_CHECK(om.at(kept.slot).state == OrderState::New);
}

// ---------------------------------------------------------------- rate limits

LLT_TEST(bucket_refuses_once_drained) {
    TokenBucket b(10.0, 1.0);
    for (int i = 0; i < 10; ++i) LLT_CHECK(b.try_take(1.0, 0));
    LLT_CHECK(!b.try_take(1.0, 0));
}

LLT_TEST(bucket_refills_over_time) {
    TokenBucket b(10.0, 1.0);
    for (int i = 0; i < 10; ++i) LLT_CHECK(b.try_take(1.0, 0));

    LLT_CHECK(b.try_take(5.0, 500'000'000));   // half a window, half the tokens
    LLT_CHECK(!b.try_take(1.0, 500'000'000));
}

LLT_TEST(bucket_does_not_overfill_while_idle) {
    TokenBucket b(10.0, 1.0);
    LLT_CHECK(b.available(100'000'000'000) <= 10.0);
}

LLT_TEST(order_limit_binds_before_the_weight_limit) {
    RateLimiter rl;
    // 300 orders per 10s is reached long before 2400 weight per minute.
    for (int i = 0; i < 300; ++i) LLT_CHECK(rl.allow_order(1.0, 0));
    LLT_CHECK(!rl.allow_order(1.0, 0));
    LLT_CHECK_EQ(rl.rejected(), 1ULL);
}

LLT_TEST(a_refused_order_spends_nothing) {
    RateLimiter rl;
    for (int i = 0; i < 300; ++i) LLT_CHECK(rl.allow_order(1.0, 0));

    LLT_CHECK(!rl.allow_order(1.0, 0));
    // The weight budget must still hold what the refused order would have cost.
    LLT_CHECK(rl.allow_request(2000.0, 0));
}
