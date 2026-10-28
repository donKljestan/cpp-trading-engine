#pragma once

// Position and profit accounting.
//
// Realised profit is booked the moment a fill reduces exposure, not when the position
// finally reaches zero. Deferring it would make a partially closed position look
// flat-to-date and hide a loss that has already happened.

#include <cstdint>

#include "llt/venue/types.hpp"

namespace llt::strat {

class Inventory {
public:
    void on_fill(Side side, Fixed px, Fixed qty, Fixed fee) noexcept {
        fees_ += fee;
        ++fills_;

        const Fixed delta = (side == Side::Buy) ? qty : Fixed{-qty.raw};

        if (position_.raw == 0) {
            position_  = delta;
            avg_entry_ = px;
            return;
        }

        const bool same_direction = (position_.raw > 0) == (delta.raw > 0);
        if (same_direction) {
            // Weighted average of the existing entry and this fill.
            const __int128_t notional = static_cast<__int128_t>(position_.raw) * avg_entry_.raw +
                                        static_cast<__int128_t>(delta.raw) * px.raw;
            position_.raw += delta.raw;
            avg_entry_.raw = static_cast<int64_t>(notional / position_.raw);
            return;
        }

        const int64_t open     = position_.raw < 0 ? -position_.raw : position_.raw;
        const int64_t incoming = delta.raw < 0 ? -delta.raw : delta.raw;
        const int64_t closed   = (incoming < open) ? incoming : open;

        // A long closes at a profit above entry; a short closes at a profit below it.
        const Fixed move = (position_.raw > 0) ? Fixed{px.raw - avg_entry_.raw}
                                               : Fixed{avg_entry_.raw - px.raw};
        realized_ += mul(move, Fixed{closed});

        position_.raw += delta.raw;

        if (position_.raw == 0) {
            avg_entry_ = Fixed{};
        } else if (incoming > open) {
            // Flipped: whatever is left over was opened at this fill's price.
            avg_entry_ = px;
        }
    }

    [[nodiscard]] Fixed    position() const noexcept { return position_; }
    [[nodiscard]] Fixed    avg_entry() const noexcept { return avg_entry_; }
    [[nodiscard]] Fixed    realized_pnl() const noexcept { return realized_; }
    [[nodiscard]] Fixed    fees_paid() const noexcept { return fees_; }
    [[nodiscard]] uint64_t fills() const noexcept { return fills_; }
    [[nodiscard]] bool     flat() const noexcept { return position_.raw == 0; }

    [[nodiscard]] Fixed unrealized_pnl(Fixed mark) const noexcept {
        if (position_.raw == 0) return Fixed{};
        const Fixed move = Fixed{mark.raw - avg_entry_.raw};
        return mul(move, position_);
    }

    // Everything banked, everything open, minus everything paid.
    [[nodiscard]] Fixed equity(Fixed mark) const noexcept {
        return realized_ + unrealized_pnl(mark) - fees_;
    }

    [[nodiscard]] Fixed notional(Fixed mark) const noexcept {
        return abs(mul(position_, mark));
    }

private:
    Fixed    position_{};
    Fixed    avg_entry_{};
    Fixed    realized_{};
    Fixed    fees_{};
    uint64_t fills_ = 0;
};

}  // namespace llt::strat
