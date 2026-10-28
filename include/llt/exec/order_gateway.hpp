#pragma once

// Order entry.
//
// This is the last code between a strategy's intention and real money, so it validates
// rather than trusts, and it refuses by default: nothing is sent unless the caller has
// explicitly asked for SendMode::Live.
//
// Every rejection here is one the exchange would have made anyway. Catching it locally
// costs a comparison; letting the venue catch it costs a round trip, a rate-limit
// token, and on repetition, the connection.

#include <cstdio>
#include <string>
#include <string_view>

#include "llt/exec/order_manager.hpp"
#include "llt/exec/rate_limit.hpp"
#include "llt/exec/ws_api.hpp"
#include "llt/venue/types.hpp"

namespace llt::exec {

enum class SendMode { DryRun, Live };

enum class OrderReject {
    None,
    NotTickAligned,
    NotStepAligned,
    BelowMinQty,
    BelowMinNotional,
    NonPositivePrice,
    NonPositiveQty,
    RateLimited,
    NoFreeSlot,
    NotConnected,
};

[[nodiscard]] constexpr const char* to_string(OrderReject r) noexcept {
    switch (r) {
        case OrderReject::None: return "ok";
        case OrderReject::NotTickAligned: return "price is not a multiple of the tick size";
        case OrderReject::NotStepAligned: return "quantity is not a multiple of the step size";
        case OrderReject::BelowMinQty: return "quantity below the minimum";
        case OrderReject::BelowMinNotional: return "notional below the minimum";
        case OrderReject::NonPositivePrice: return "price is not positive";
        case OrderReject::NonPositiveQty: return "quantity is not positive";
        case OrderReject::RateLimited: return "would exceed a rate limit";
        case OrderReject::NoFreeSlot: return "no free order slot";
        case OrderReject::NotConnected: return "no authenticated session";
    }
    return "?";
}

[[nodiscard]] constexpr OrderReject validate(const Instrument& inst, Fixed px,
                                             Fixed qty) noexcept {
    if (px.raw <= 0) return OrderReject::NonPositivePrice;
    if (qty.raw <= 0) return OrderReject::NonPositiveQty;
    if (inst.tick_size.raw > 0 && px.raw % inst.tick_size.raw != 0) {
        return OrderReject::NotTickAligned;
    }
    if (inst.step_size.raw > 0 && qty.raw % inst.step_size.raw != 0) {
        return OrderReject::NotStepAligned;
    }
    if (inst.min_qty.raw > 0 && qty.raw < inst.min_qty.raw) return OrderReject::BelowMinQty;
    if (inst.min_notional.raw > 0 && mul(px, qty).raw < inst.min_notional.raw) {
        return OrderReject::BelowMinNotional;
    }
    return OrderReject::None;
}

[[nodiscard]] inline std::string format_amount(Fixed v) {
    char         buf[kFixedMaxChars];
    const size_t n = format_fixed(v, buf, sizeof(buf));
    return std::string(buf, n);
}

// GTX is post-only: the venue rejects the order outright rather than letting it cross
// and charging the taker fee. For a market maker a rejection is the correct outcome.
[[nodiscard]] inline std::string build_place_params(const Instrument& inst, Side side,
                                                    Fixed px, Fixed qty,
                                                    std::string_view client_id,
                                                    bool post_only = true) {
    return Params{}
        .s("symbol", inst.symbol.view())
        .s("side", side == Side::Buy ? "BUY" : "SELL")
        .s("type", "LIMIT")
        .s("timeInForce", post_only ? "GTX" : "GTC")
        .s("quantity", format_amount(qty))
        .s("price", format_amount(px))
        .s("newClientOrderId", client_id)
        .finish_stamped();
}

[[nodiscard]] inline std::string build_cancel_params(const Instrument& inst,
                                                     std::string_view client_id) {
    return Params{}
        .s("symbol", inst.symbol.view())
        .s("origClientOrderId", client_id)
        .finish_stamped();
}

struct GatewayStats {
    uint64_t placed    = 0;
    uint64_t canceled  = 0;
    uint64_t rejected  = 0;
    uint64_t suppressed = 0;  // dry run
};

class OrderGateway {
public:
    OrderGateway(WsApiClient& api, OrderManager& orders, RateLimiter& limits,
                 SendMode mode) noexcept
        : api_(api), orders_(orders), limits_(limits), mode_(mode) {}

    [[nodiscard]] SendMode mode() const noexcept { return mode_; }
    [[nodiscard]] const GatewayStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string&  last_request() const noexcept { return last_request_; }
    [[nodiscard]] const char*         last_error() const noexcept { return last_error_; }

    OrderReject place(const Instrument& inst, Side side, Fixed px, Fixed qty,
                      uint64_t now_ns, OrderRef& out) {
        // Checked before anything is consumed: a slot or a rate-limit token spent on
        // an order that cannot be sent is a slot the engine then believes is resting.
        if (mode_ == SendMode::Live && !api_.authenticated()) {
            ++stats_.rejected;
            last_error_ = to_string(OrderReject::NotConnected);
            return OrderReject::NotConnected;
        }

        const OrderReject why = validate(inst, px, qty);
        if (why != OrderReject::None) {
            ++stats_.rejected;
            last_error_ = to_string(why);
            return why;
        }
        if (!limits_.allow_order(1.0, now_ns)) {
            ++stats_.rejected;
            last_error_ = to_string(OrderReject::RateLimited);
            return OrderReject::RateLimited;
        }
        if (!orders_.reserve(side, px, qty, now_ns, out)) {
            ++stats_.rejected;
            last_error_ = to_string(OrderReject::NoFreeSlot);
            return OrderReject::NoFreeSlot;
        }

        last_request_ = build_place_params(inst, side, px, qty, out.client_id);

        if (mode_ == SendMode::DryRun) {
            ++stats_.suppressed;
            // The slot is released again so a dry run does not exhaust the pool.
            static_cast<void>(orders_.on_update(out.client_id, OrderState::Canceled,
                                                Fixed{}, Fixed{}, now_ns));
            return OrderReject::None;
        }

        WsApiResponse resp;
        if (!api_.request("order.place", last_request_, 5'000, resp) || !resp.ok()) {
            ++stats_.rejected;
            last_error_ = WsApiClient::last_error();
            // The venue never acknowledged it, so neither do we.
            static_cast<void>(orders_.on_update(out.client_id, OrderState::Rejected,
                                                Fixed{}, Fixed{}, now_ns));
            return OrderReject::NotConnected;
        }

        ++stats_.placed;
        return OrderReject::None;
    }

    OrderReject cancel(const Instrument& inst, const OrderRef& ref, uint64_t now_ns) {
        if (mode_ == SendMode::Live && !api_.authenticated()) {
            ++stats_.rejected;
            last_error_ = to_string(OrderReject::NotConnected);
            return OrderReject::NotConnected;
        }
        if (!limits_.allow_request(1.0, now_ns)) {
            ++stats_.rejected;
            last_error_ = to_string(OrderReject::RateLimited);
            return OrderReject::RateLimited;
        }

        orders_.request_cancel(ref.slot);
        last_request_ = build_cancel_params(inst, ref.client_id);

        if (mode_ == SendMode::DryRun) {
            ++stats_.suppressed;
            return OrderReject::None;
        }

        WsApiResponse resp;
        if (!api_.request("order.cancel", last_request_, 5'000, resp) || !resp.ok()) {
            ++stats_.rejected;
            last_error_ = WsApiClient::last_error();
            return OrderReject::NotConnected;
        }

        ++stats_.canceled;
        return OrderReject::None;
    }

private:
    WsApiClient&  api_;
    OrderManager& orders_;
    RateLimiter&  limits_;
    SendMode      mode_;
    GatewayStats  stats_;
    std::string   last_request_;
    const char*   last_error_ = "";
};

}  // namespace llt::exec
