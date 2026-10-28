#pragma once

// Pre-trade limits and the halt latch.
//
// Two different things are deliberately kept apart here.
//
// A *veto* refuses one order and clears by itself once the condition that caused it
// goes away: data arrives again, the position comes back inside its band.
//
// A *halt* latches. Only an operator calling reset() clears it. Daily loss and a
// reject storm are halts because the condition that triggered them says our model of
// the world is wrong, and a limit that can un-trip itself will un-trip at the worst
// possible moment — an unrealised bounce makes equity look survivable again and
// trading resumes exactly when it is least likely to be right.
//
// Scope: this layer owns *exposure*. It does not re-check tick size or lot size —
// that is the gateway's job (llt/exec/order_gateway.hpp). Splitting them keeps each
// check in exactly one place.

#include <cstdint>

#include "llt/core/fixed.hpp"
#include "llt/venue/types.hpp"

namespace llt::risk {

enum class Veto {
    None,
    Halted,
    NoMarketData,
    StaleData,
    BadReference,
    PriceOutOfBand,
    OrderTooLarge,
    PositionLimit,
    NotionalLimit,
};

[[nodiscard]] constexpr const char* to_string(Veto v) noexcept {
    switch (v) {
        case Veto::None: return "none";
        case Veto::Halted: return "halted";
        case Veto::NoMarketData: return "no market data";
        case Veto::StaleData: return "stale market data";
        case Veto::BadReference: return "bad reference price";
        case Veto::PriceOutOfBand: return "price outside sanity band";
        case Veto::OrderTooLarge: return "order notional too large";
        case Veto::PositionLimit: return "would breach position limit";
        case Veto::NotionalLimit: return "would breach notional limit";
    }
    return "unknown";
}

enum class HaltReason {
    None,
    MaxLoss,
    RejectStorm,
    KillSwitch,
    StaleDataTimeout,
    Manual,
};

[[nodiscard]] constexpr const char* to_string(HaltReason r) noexcept {
    switch (r) {
        case HaltReason::None: return "none";
        case HaltReason::MaxLoss: return "maximum loss reached";
        case HaltReason::RejectStorm: return "too many consecutive rejects";
        case HaltReason::KillSwitch: return "kill switch engaged";
        case HaltReason::StaleDataTimeout: return "market data gone too long";
        case HaltReason::Manual: return "halted by operator";
    }
    return "unknown";
}

// A default-constructed config permits nothing. Zero means "deny", not "unlimited",
// so a config somebody forgot to fill in stops the engine instead of waving it
// through with no ceiling at all.
struct RiskConfig {
    Fixed    max_position{};        // absolute, base units
    Fixed    max_notional{};        // absolute, quote units
    Fixed    max_order_notional{};  // per single order, quote units
    Fixed    max_loss{};            // positive magnitude; equity <= -max_loss halts
    double   max_price_deviation_bps = 200.0;
    uint64_t stale_data_ms           = 2'000;
    uint64_t halt_on_stale_ms        = 60'000;  // a gap this long is not a hiccup
    uint32_t max_consecutive_rejects = 10;
};

class RiskManager {
public:
    explicit RiskManager(RiskConfig cfg) noexcept : cfg_(cfg) {}

    [[nodiscard]] const RiskConfig& config() const noexcept { return cfg_; }

    void on_market_data(uint64_t now_ns) noexcept {
        last_md_ns_ = now_ns;
        have_md_    = true;
    }

    // Equity is realised plus unrealised minus fees, so a position that has not been
    // closed yet still counts against the loss limit. Waiting for the close would let
    // a losing position hide behind the fact that it is still open.
    void on_equity(Fixed equity) noexcept {
        if (equity < worst_equity_) worst_equity_ = equity;
        if (cfg_.max_loss.raw > 0 && equity.raw <= -cfg_.max_loss.raw) {
            halt(HaltReason::MaxLoss);
        }
    }

    // Repeated rejections mean our picture of the venue is wrong — stale rules, an
    // expired session, a symbol that stopped trading. Hammering on earns a ban.
    void on_reject() noexcept {
        ++consecutive_rejects_;
        if (cfg_.max_consecutive_rejects > 0 &&
            consecutive_rejects_ >= cfg_.max_consecutive_rejects) {
            halt(HaltReason::RejectStorm);
        }
    }

    void on_accept() noexcept { consecutive_rejects_ = 0; }

    [[nodiscard]] bool data_stale(uint64_t now_ns) const noexcept {
        if (!have_md_) return true;
        if (now_ns <= last_md_ns_) return false;
        return (now_ns - last_md_ns_) > cfg_.stale_data_ms * uint64_t{1'000'000};
    }

    // Checked on the housekeeping path: a brief gap is a veto, a long one means the
    // feed is not coming back without help.
    void poll(uint64_t now_ns) noexcept {
        if (!have_md_ || halted()) return;
        if (cfg_.halt_on_stale_ms == 0) return;
        if (now_ns > last_md_ns_ &&
            (now_ns - last_md_ns_) > cfg_.halt_on_stale_ms * uint64_t{1'000'000}) {
            halt(HaltReason::StaleDataTimeout);
        }
    }

    // Resting orders are priced off a book we can no longer see, so a stale feed is
    // not only a reason to stop sending — it is a reason to pull what is already out.
    [[nodiscard]] bool should_cancel_all(uint64_t now_ns) const noexcept {
        return halted() || data_stale(now_ns);
    }

    [[nodiscard]] Veto check_order(Side side, Fixed px, Fixed qty, Fixed ref_px,
                                   Fixed position, uint64_t now_ns) noexcept {
        const Veto v = evaluate(side, px, qty, ref_px, position, now_ns);
        if (v != Veto::None) {
            ++vetoes_;
            last_veto_ = v;
        }
        return v;
    }

    void halt(HaltReason r) noexcept {
        if (halt_reason_ == HaltReason::None) halt_reason_ = r;
    }

    // Deliberately the only way out, and deliberately not called by any check.
    void reset() noexcept {
        halt_reason_         = HaltReason::None;
        consecutive_rejects_ = 0;
    }

    [[nodiscard]] bool halted() const noexcept { return halt_reason_ != HaltReason::None; }
    [[nodiscard]] HaltReason halt_reason() const noexcept { return halt_reason_; }
    [[nodiscard]] Veto       last_veto() const noexcept { return last_veto_; }
    [[nodiscard]] uint64_t   vetoes() const noexcept { return vetoes_; }
    [[nodiscard]] uint32_t consecutive_rejects() const noexcept { return consecutive_rejects_; }
    [[nodiscard]] Fixed    worst_equity() const noexcept { return worst_equity_; }

private:
    [[nodiscard]] Veto evaluate(Side side, Fixed px, Fixed qty, Fixed ref_px,
                                Fixed position, uint64_t now_ns) const noexcept {
        if (halted()) return Veto::Halted;
        if (!have_md_) return Veto::NoMarketData;
        if (data_stale(now_ns)) return Veto::StaleData;
        if (ref_px.raw <= 0) return Veto::BadReference;

        if (cfg_.max_price_deviation_bps > 0.0) {
            const Fixed off  = abs(Fixed{px.raw - ref_px.raw});
            const Fixed bps  = div(off, ref_px) * int64_t{10'000};
            const Fixed band = Fixed::from_double(cfg_.max_price_deviation_bps);
            if (bps > band) return Veto::PriceOutOfBand;
        }

        if (mul(abs(px), abs(qty)) > cfg_.max_order_notional) return Veto::OrderTooLarge;

        // The position this order could create, not the one we hold. Checking the
        // current position lets an order push us straight through the ceiling.
        const Fixed delta = (side == Side::Buy) ? qty : Fixed{-qty.raw};
        const Fixed after = position + delta;

        if (abs(after) > cfg_.max_position) return Veto::PositionLimit;
        if (mul(abs(after), ref_px) > cfg_.max_notional) return Veto::NotionalLimit;

        return Veto::None;
    }

    RiskConfig cfg_;
    uint64_t   last_md_ns_          = 0;
    bool       have_md_             = false;
    HaltReason halt_reason_         = HaltReason::None;
    Veto       last_veto_           = Veto::None;
    uint64_t   vetoes_              = 0;
    uint32_t   consecutive_rejects_ = 0;
    Fixed      worst_equity_{};
};

}  // namespace llt::risk
