#pragma once

// Market data events, as they travel between threads.
//
// Every struct here is trivially copyable and fixed size so it can go straight into an
// SpscRing with no allocation and no indirection.

#include <cstdint>

#include "llt/venue/types.hpp"

namespace llt {

struct PriceLevel {
    Fixed px;
    Fixed qty;
};

// Timestamps carried by every event.
//   exchange_ms - the venue's own clock (field E), only comparable to a disciplined local clock
//   recv_tsc    - local TSC at the moment the frame was read, the basis for tick-to-decision
struct EventTime {
    uint64_t exchange_ms = 0;
    uint64_t transact_ms = 0;
    uint64_t recv_tsc    = 0;
};

// Best bid and offer. Binance pushes this on every top-of-book change rather than on a
// timer, which makes it the lowest-latency signal the venue offers.
struct BookTicker {
    Symbol    symbol;
    EventTime time;
    uint64_t  update_id = 0;
    Fixed     bid_px;
    Fixed     bid_qty;
    Fixed     ask_px;
    Fixed     ask_qty;

    [[nodiscard]] Fixed spread() const noexcept { return ask_px - bid_px; }

    [[nodiscard]] Fixed mid() const noexcept {
        return Fixed::from_raw((bid_px.raw + ask_px.raw) / 2);
    }

    // Size-weighted mid. Leans toward the side with less resting size, which is the
    // side the next trade is more likely to move.
    [[nodiscard]] Fixed microprice() const noexcept {
        const __int128_t denom = static_cast<__int128_t>(bid_qty.raw) + ask_qty.raw;
        if (denom == 0) return mid();
        const __int128_t num = static_cast<__int128_t>(bid_qty.raw) * ask_px.raw +
                               static_cast<__int128_t>(ask_qty.raw) * bid_px.raw;
        return Fixed::from_raw(static_cast<int64_t>(num / denom));
    }
};

// Top-N book from a partial depth stream. Self-contained: each message replaces the
// previous one, so there is no sequence state to keep and nothing to resynchronise.
struct DepthSnapshot {
    static constexpr size_t kMaxLevels = 20;

    Symbol    symbol;
    EventTime time;
    uint64_t  first_update_id = 0;  // U
    uint64_t  final_update_id = 0;  // u
    uint64_t  prev_final_id   = 0;  // pu

    PriceLevel bids[kMaxLevels] = {};
    PriceLevel asks[kMaxLevels] = {};
    uint8_t    n_bids           = 0;
    uint8_t    n_asks           = 0;

    // Resting size imbalance over the top `depth` levels, in [-1, 1].
    // Positive means more size bid than offered.
    [[nodiscard]] double imbalance(size_t depth) const noexcept {
        __int128_t b = 0;
        __int128_t a = 0;
        for (size_t i = 0; i < depth && i < n_bids; ++i) b += bids[i].qty.raw;
        for (size_t i = 0; i < depth && i < n_asks; ++i) a += asks[i].qty.raw;
        const __int128_t total = b + a;
        if (total == 0) return 0.0;
        return static_cast<double>(static_cast<int64_t>(b - a)) /
               static_cast<double>(static_cast<int64_t>(total));
    }
};

struct Trade {
    Symbol    symbol;
    EventTime time;
    uint64_t  id = 0;
    Fixed     px;
    Fixed     qty;
    bool      buyer_is_maker = false;

    // Which side crossed the spread. This is the informative part of a print.
    [[nodiscard]] Side aggressor() const noexcept {
        return buyer_is_maker ? Side::Sell : Side::Buy;
    }
};

}  // namespace llt
