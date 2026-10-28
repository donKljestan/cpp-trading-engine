#pragma once

// Decoding of the private event stream.
//
// These are the only messages that say what actually happened to an order. Everything
// else in the system is a belief; this is the record.

#include <cstring>
#include <string_view>

#include "llt/exec/order.hpp"
#include "llt/venue/json_scan.hpp"
#include "llt/venue/types.hpp"

namespace llt::binance {

// What the venue reports about one order, at one moment.
struct ExecReport {
    Symbol           symbol;
    char             client_id[40] = {};
    exec::OrderState state         = exec::OrderState::Idle;
    Side             side          = Side::Buy;
    Fixed            cum_filled{};   // z, cumulative
    Fixed            last_qty{};     // l, this execution only
    Fixed            last_px{};      // L
    Fixed            commission{};   // n
    uint64_t         order_id    = 0;
    uint64_t         event_ms    = 0;
    uint64_t         transact_ms = 0;
};

struct PositionUpdate {
    Symbol symbol;
    Fixed  amount{};      // pa, signed
    Fixed  entry_price{};  // ep
    Fixed  unrealized{};   // up
};

struct AccountUpdate {
    static constexpr size_t kMaxPositions = 16;

    uint64_t       event_ms    = 0;
    uint64_t       transact_ms = 0;
    Fixed          wallet_usdt{};
    PositionUpdate positions[kMaxPositions] = {};
    uint8_t        n_positions              = 0;
};

[[nodiscard]] inline bool order_state_from(std::string_view s,
                                           exec::OrderState& out) noexcept {
    using exec::OrderState;
    if (s == "NEW") {
        out = OrderState::New;
    } else if (s == "PARTIALLY_FILLED") {
        out = OrderState::PartiallyFilled;
    } else if (s == "FILLED") {
        out = OrderState::Filled;
    } else if (s == "CANCELED") {
        out = OrderState::Canceled;
    } else if (s == "EXPIRED") {
        // A post-only order that would have crossed, or one the venue aged out.
        out = OrderState::Expired;
    } else if (s == "REJECTED" || s == "EXPIRED_IN_MATCH") {
        out = OrderState::Rejected;
    } else {
        // NEW_INSURANCE and NEW_ADL are liquidation machinery, not our order moving.
        // Guessing a state for an unrecognised status is how a position goes missing.
        return false;
    }
    return true;
}

// {"e":"ORDER_TRADE_UPDATE","E":..,"T":..,"o":{"s":..,"c":..,"S":..,"X":..,"z":..,..}}
[[nodiscard]] inline bool parse_order_update(std::string_view json,
                                             ExecReport& out) noexcept {
    std::string_view order;
    if (!json::find_field(json, "o", order)) return false;

    std::string_view value;
    if (json::find_field(json, "E", value)) {
        static_cast<void>(json::parse_uint(value, out.event_ms));
    }
    if (json::find_field(json, "T", value)) {
        static_cast<void>(json::parse_uint(value, out.transact_ms));
    }

    json::ObjectScanner fields(order);
    std::string_view    key;

    bool have_client = false;
    bool have_state  = false;
    bool have_side   = false;

    while (fields.next(key, value)) {
        if (key == "s") {
            out.symbol = Symbol(json::unquote(value));
        } else if (key == "c") {
            const std::string_view id = json::unquote(value);
            const size_t n = (id.size() < sizeof(out.client_id) - 1) ? id.size()
                                                                     : sizeof(out.client_id) - 1;
            std::memcpy(out.client_id, id.data(), n);
            out.client_id[n] = '\0';
            have_client      = true;
        } else if (key == "S") {
            const std::string_view side = json::unquote(value);
            if (side == "BUY") {
                out.side = Side::Buy;
                have_side = true;
            } else if (side == "SELL") {
                out.side = Side::Sell;
                have_side = true;
            }
        } else if (key == "X") {
            have_state = order_state_from(json::unquote(value), out.state);
        } else if (key == "z") {
            static_cast<void>(json::parse_decimal(value, out.cum_filled));
        } else if (key == "l") {
            static_cast<void>(json::parse_decimal(value, out.last_qty));
        } else if (key == "L") {
            static_cast<void>(json::parse_decimal(value, out.last_px));
        } else if (key == "n") {
            static_cast<void>(json::parse_decimal(value, out.commission));
        } else if (key == "i") {
            static_cast<void>(json::parse_uint(value, out.order_id));
        }
    }

    return have_client && have_state && have_side;
}

// {"e":"ACCOUNT_UPDATE","E":..,"T":..,"a":{"B":[{"a":"USDT","wb":".."}],"P":[..]}}
[[nodiscard]] inline bool parse_account_update(std::string_view json,
                                               AccountUpdate& out) noexcept {
    std::string_view account;
    if (!json::find_field(json, "a", account)) return false;

    std::string_view value;
    if (json::find_field(json, "E", value)) {
        static_cast<void>(json::parse_uint(value, out.event_ms));
    }
    if (json::find_field(json, "T", value)) {
        static_cast<void>(json::parse_uint(value, out.transact_ms));
    }

    std::string_view balances;
    if (json::find_field(account, "B", balances)) {
        json::ArrayScanner entries(balances);
        std::string_view   entry;
        while (entries.next(entry)) {
            std::string_view asset;
            if (!json::find_field(entry, "a", asset)) continue;
            if (json::unquote(asset) != "USDT") continue;
            if (json::find_field(entry, "wb", value)) {
                static_cast<void>(json::parse_decimal(value, out.wallet_usdt));
            }
        }
    }

    std::string_view positions;
    if (json::find_field(account, "P", positions)) {
        json::ArrayScanner entries(positions);
        std::string_view   entry;
        while (entries.next(entry) && out.n_positions < AccountUpdate::kMaxPositions) {
            PositionUpdate p;
            std::string_view symbol;
            if (!json::find_field(entry, "s", symbol)) continue;
            p.symbol = Symbol(json::unquote(symbol));

            if (json::find_field(entry, "pa", value)) {
                static_cast<void>(json::parse_decimal(value, p.amount));
            }
            if (json::find_field(entry, "ep", value)) {
                static_cast<void>(json::parse_decimal(value, p.entry_price));
            }
            if (json::find_field(entry, "up", value)) {
                static_cast<void>(json::parse_decimal(value, p.unrealized));
            }
            out.positions[out.n_positions++] = p;
        }
    }
    return true;
}

}  // namespace llt::binance
