#pragma once

// Order book of our own orders.
//
// A fixed pool of slots, never allocating. The client order id carries the slot index,
// so routing an exchange update is an array index rather than a hash lookup, and a
// sequence number guards against a stale update landing on a reused slot.

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "llt/exec/order.hpp"

namespace llt::exec {

inline constexpr size_t kMaxOrders = 64;

// "llt<slot:03>_<seq:012>" - 19 characters, inside the venue's 36-character limit and
// its allowed character set.
inline constexpr size_t kClientIdLen = 19;

struct OrderRef {
    size_t   slot = 0;
    uint64_t seq  = 0;
    char     client_id[kClientIdLen + 1] = {};
};

[[nodiscard]] inline bool parse_client_id(std::string_view id, size_t& slot,
                                          uint64_t& seq) noexcept {
    if (id.size() != kClientIdLen || id.compare(0, 3, "llt") != 0 || id[6] != '_') {
        return false;
    }

    size_t s = 0;
    for (size_t i = 3; i < 6; ++i) {
        if (id[i] < '0' || id[i] > '9') return false;
        s = s * 10 + static_cast<size_t>(id[i] - '0');
    }

    uint64_t q = 0;
    for (size_t i = 7; i < kClientIdLen; ++i) {
        if (id[i] < '0' || id[i] > '9') return false;
        q = q * 10 + static_cast<uint64_t>(id[i] - '0');
    }

    slot = s;
    seq  = q;
    return true;
}

class OrderManager {
public:
    // Takes a free slot. A slot is free when it is empty or holds a finished order;
    // the terminal state is kept rather than cleared, so a late duplicate for that
    // order is still recognised as belonging to something already over.
    [[nodiscard]] bool reserve(Side side, Fixed px, Fixed qty, uint64_t now_ns,
                               OrderRef& out) noexcept {
        for (size_t i = 0; i < kMaxOrders; ++i) {
            Order& o = orders_[i];
            if (o.state != OrderState::Idle && !is_terminal(o.state)) continue;

            o                  = Order{};
            o.seq              = ++seq_counter_;
            o.state            = OrderState::PendingNew;
            o.side             = side;
            o.px               = px;
            o.qty              = qty;
            o.created_ns       = now_ns;
            o.updated_ns       = now_ns;

            out.slot = i;
            out.seq  = o.seq;
            std::snprintf(out.client_id, sizeof(out.client_id), "llt%03zu_%012llu", i,
                          static_cast<unsigned long long>(o.seq));
            ++live_;
            return true;
        }
        return false;
    }

    // Routes an exchange update. Unknown or stale ids are ignored rather than guessed
    // at: an update for an order we do not recognise is never ours to act on.
    FillDelta on_update(std::string_view client_id, OrderState state, Fixed cum_filled,
                        Fixed trade_px, uint64_t now_ns) noexcept {
        FillDelta delta;

        size_t   slot = 0;
        uint64_t seq  = 0;
        if (!parse_client_id(client_id, slot, seq) || slot >= kMaxOrders) {
            ++unknown_updates_;
            return delta;
        }

        Order& o = orders_[slot];
        if (o.seq != seq) {
            // The slot has been recycled; this belongs to the order that used to live here.
            ++stale_updates_;
            return delta;
        }

        const bool was_live = is_live(o.state);
        delta               = o.apply(state, cum_filled, trade_px, now_ns);

        if (was_live && is_terminal(o.state)) --live_;
        return delta;
    }

    void request_cancel(size_t slot) noexcept {
        if (slot < kMaxOrders) orders_[slot].request_cancel();
    }

    [[nodiscard]] const Order& at(size_t slot) const noexcept { return orders_[slot]; }
    [[nodiscard]] size_t       live() const noexcept { return live_; }
    [[nodiscard]] uint64_t     unknown_updates() const noexcept { return unknown_updates_; }
    [[nodiscard]] uint64_t     stale_updates() const noexcept { return stale_updates_; }

    // Reconciliation against an exchange snapshot. Anything we believe is live but the
    // exchange does not mention has been lost, and continuing to believe in it is how
    // a position appears from nowhere.
    void begin_reconcile() noexcept {
        for (auto& seen : seen_) seen = false;
    }

    void saw(std::string_view client_id) noexcept {
        size_t   slot = 0;
        uint64_t seq  = 0;
        if (parse_client_id(client_id, slot, seq) && slot < kMaxOrders &&
            orders_[slot].seq == seq) {
            seen_[slot] = true;
        }
    }

    // Returns how many orders were dropped as orphaned.
    size_t end_reconcile() noexcept {
        size_t dropped = 0;
        for (size_t i = 0; i < kMaxOrders; ++i) {
            if (!is_live(orders_[i].state) || seen_[i]) continue;
            orders_[i].state = OrderState::Idle;
            --live_;
            ++dropped;
        }
        orphaned_ += dropped;
        return dropped;
    }

    [[nodiscard]] uint64_t orphaned() const noexcept { return orphaned_; }

private:
    std::array<Order, kMaxOrders> orders_{};
    std::array<bool, kMaxOrders>  seen_{};

    uint64_t seq_counter_     = 0;
    size_t   live_            = 0;
    uint64_t unknown_updates_ = 0;
    uint64_t stale_updates_   = 0;
    uint64_t orphaned_        = 0;
};

}  // namespace llt::exec
