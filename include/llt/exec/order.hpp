#pragma once

// Order state.
//
// The exchange reports an order's life through two channels that race: the reply to
// order.place and the user data stream. An update therefore routinely arrives before
// the acknowledgement of the order it concerns, so every transition here is written to
// be safe when applied out of order or twice.

#include <cstdint>

#include "llt/venue/types.hpp"

namespace llt::exec {

enum class OrderState : uint8_t {
    Idle,             // the slot holds no order
    PendingNew,       // sent, not yet acknowledged
    New,              // resting on the book
    PartiallyFilled,  // resting, partly executed
    Filled,
    Canceled,
    Rejected,
    // A post-only order that would have crossed. The venue reports this as EXPIRED;
    // it is the normal outcome of GTX protecting us from paying the taker fee, not an
    // error worth alerting on.
    Expired,
};

[[nodiscard]] constexpr bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Filled || s == OrderState::Canceled ||
           s == OrderState::Rejected || s == OrderState::Expired;
}

[[nodiscard]] constexpr bool is_live(OrderState s) noexcept {
    return s == OrderState::PendingNew || s == OrderState::New ||
           s == OrderState::PartiallyFilled;
}

[[nodiscard]] constexpr const char* to_string(OrderState s) noexcept {
    switch (s) {
        case OrderState::Idle: return "IDLE";
        case OrderState::PendingNew: return "PENDING_NEW";
        case OrderState::New: return "NEW";
        case OrderState::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderState::Filled: return "FILLED";
        case OrderState::Canceled: return "CANCELED";
        case OrderState::Rejected: return "REJECTED";
        case OrderState::Expired: return "EXPIRED";
    }
    return "?";
}

// An order only ever moves forward through its life. Ranking the states makes a stale
// update a no-op instead of a resurrection.
[[nodiscard]] constexpr uint8_t rank(OrderState s) noexcept {
    switch (s) {
        case OrderState::Idle: return 0;
        case OrderState::PendingNew: return 1;
        case OrderState::New: return 2;
        case OrderState::PartiallyFilled: return 3;
        default: return 4;  // terminal
    }
}

// What an update actually changed, for the caller to pass on to inventory.
struct FillDelta {
    bool  occurred = false;
    Side  side     = Side::Buy;
    Fixed qty{};  // newly executed amount, not the cumulative figure
    Fixed px{};
};

struct Order {
    uint64_t   seq   = 0;  // distinguishes reuses of the same slot
    OrderState state = OrderState::Idle;
    Side       side  = Side::Buy;
    Fixed      px{};
    Fixed      qty{};
    Fixed      cum_filled{};
    Fixed      last_px{};
    bool       cancel_requested = false;
    uint64_t   created_ns       = 0;
    uint64_t   updated_ns       = 0;

    [[nodiscard]] Fixed remaining() const noexcept {
        return Fixed{qty.raw - cum_filled.raw};
    }

    // Cancellation is a request about an order, not a stage of its life: a fill can
    // still land after the cancel is sent. Keeping it separate from the state avoids
    // a transition table where that looks like going backwards.
    void request_cancel() noexcept {
        if (is_live(state)) cancel_requested = true;
    }

    // `cum` is the venue's cumulative executed quantity, which is why a replayed or
    // duplicated update adds nothing.
    FillDelta apply(OrderState next, Fixed cum, Fixed trade_px, uint64_t now_ns) noexcept {
        FillDelta delta;
        if (is_terminal(state)) return delta;

        if (cum.raw > cum_filled.raw) {
            delta.occurred = true;
            delta.side     = side;
            delta.qty      = Fixed{cum.raw - cum_filled.raw};
            delta.px       = (trade_px.raw > 0) ? trade_px : px;
            cum_filled     = cum;
            last_px        = delta.px;
        }

        if (rank(next) >= rank(state)) state = next;
        updated_ns = now_ns;
        return delta;
    }
};

}  // namespace llt::exec
