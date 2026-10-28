#pragma once

// Quote generation.
//
// Inspired by Avellaneda-Stoikov rather than a literal implementation of it: the
// textbook form needs a risk-aversion parameter calibrated in utility units, which
// nothing here can supply honestly. What survives is the shape - a fair value, a
// reservation price pushed away from inventory, and a spread bounded below by cost.
//
// Everything is computed in basis points of fair value, so the same parameters mean
// the same thing on a $80,000 instrument and a $0.001 one.

#include <algorithm>
#include <cmath>

#include "llt/strat/inventory.hpp"
#include "llt/strat/signals.hpp"
#include "llt/venue/types.hpp"

namespace llt::strat {

struct QuoterConfig {
    Fixed order_qty;     // base units per side
    Fixed max_position;  // absolute, base units

    // A round trip pays the maker fee twice, so the full spread must clear it.
    double fee_bps         = 4.0;
    double edge_buffer_bps = 1.0;

    // How far ahead the quote is exposed, and how much of that risk to charge for.
    double vol_horizon_s  = 5.0;
    double vol_multiplier = 1.0;

    // Inventory push, as a fraction of a volatility-horizon move at full position.
    double inventory_gamma = 1.0;

    // Beyond these the book is telling us which way it is about to go, and a resting
    // quote on the wrong side is a donation.
    double max_abs_obi = 0.75;
    double max_abs_tfi = 0.75;

    // Requoting costs rate limit and queue position, so it needs to be worth it.
    int64_t  min_requote_ticks = 1;
    uint64_t min_requote_ms    = 100;
};

struct Quote {
    bool  bid_active = false;
    bool  ask_active = false;
    Fixed bid_px{};
    Fixed ask_px{};
    Fixed qty{};

    [[nodiscard]] bool any() const noexcept { return bid_active || ask_active; }
};

class Quoter {
public:
    explicit Quoter(QuoterConfig cfg) noexcept : cfg_(cfg) {}

    [[nodiscard]] const QuoterConfig& config() const noexcept { return cfg_; }

    // Half the distance between the two quotes, in basis points. Never below the cost
    // of trading: a quote tighter than its own fees loses money on every fill.
    [[nodiscard]] double half_spread_bps(double vol_bps) const noexcept {
        const double floor_bps = cfg_.fee_bps / 2.0 + cfg_.edge_buffer_bps;
        const double risk_bps =
            cfg_.vol_multiplier * vol_bps * std::sqrt(cfg_.vol_horizon_s);
        return std::max(floor_bps, risk_bps);
    }

    // Positive inventory pushes both quotes down, making a sale more likely and a
    // further purchase less so.
    [[nodiscard]] double inventory_skew_bps(const Inventory& inv,
                                            double           vol_bps) const noexcept {
        if (cfg_.max_position.raw == 0) return 0.0;

        const double fraction = static_cast<double>(inv.position().raw) /
                                static_cast<double>(cfg_.max_position.raw);
        const double horizon_move = vol_bps * std::sqrt(cfg_.vol_horizon_s);
        return cfg_.inventory_gamma * fraction * horizon_move;
    }

    [[nodiscard]] Quote compute(const Signals& sig, const Inventory& inv,
                                const Instrument& inst) const noexcept {
        Quote q;
        if (!sig.ready() || inst.tick_size.raw <= 0) return q;

        const double vol = sig.vol_bps();
        if (vol <= 0.0) return q;  // not enough samples to price risk

        const Fixed fv = sig.fair_value();
        if (fv.raw <= 0) return q;

        const double half = half_spread_bps(vol);
        const double skew = inventory_skew_bps(inv, vol);

        q.qty      = cfg_.order_qty;
        q.bid_px   = round_down(offset(fv, -half - skew), inst.tick_size);
        q.ask_px   = round_up(offset(fv, half - skew), inst.tick_size);
        q.bid_active = true;
        q.ask_active = true;

        // Post-only orders are rejected if they would cross, so keep each side strictly
        // inside the opposite touch.
        const Fixed best_bid = sig.book().bid_px;
        const Fixed best_ask = sig.book().ask_px;
        if (best_ask.raw > 0) {
            q.bid_px = min(q.bid_px, Fixed{best_ask.raw - inst.tick_size.raw});
        }
        if (best_bid.raw > 0) {
            q.ask_px = max(q.ask_px, Fixed{best_bid.raw + inst.tick_size.raw});
        }

        // One-sided once the book is clearly leaning: quote only the side that would
        // be trading with the flow rather than in front of it.
        const double obi = sig.obi();
        const double tfi = sig.tfi();
        if (obi > cfg_.max_abs_obi || tfi > cfg_.max_abs_tfi) q.ask_active = false;
        if (obi < -cfg_.max_abs_obi || tfi < -cfg_.max_abs_tfi) q.bid_active = false;

        // At the position limit, only the side that reduces exposure may quote.
        if (cfg_.max_position.raw > 0) {
            if (inv.position().raw >= cfg_.max_position.raw) q.bid_active = false;
            if (inv.position().raw <= -cfg_.max_position.raw) q.ask_active = false;
        }

        if (q.bid_px.raw <= 0) q.bid_active = false;
        if (q.ask_px.raw <= 0) q.ask_active = false;
        return q;
    }

    // Replacing a quote costs rate limit and surrenders queue position, so it has to
    // buy something.
    [[nodiscard]] bool should_replace(const Quote& live, const Quote& desired,
                                      Fixed tick, uint64_t now_ns,
                                      uint64_t last_change_ns) const noexcept {
        if (live.bid_active != desired.bid_active) return true;
        if (live.ask_active != desired.ask_active) return true;
        if (!desired.any()) return false;

        if (now_ns < last_change_ns + cfg_.min_requote_ms * uint64_t{1'000'000}) {
            return false;
        }

        const int64_t threshold = cfg_.min_requote_ticks * tick.raw;
        const int64_t bid_move  = std::abs(desired.bid_px.raw - live.bid_px.raw);
        const int64_t ask_move  = std::abs(desired.ask_px.raw - live.ask_px.raw);
        return bid_move >= threshold || ask_move >= threshold;
    }

private:
    [[nodiscard]] static Fixed offset(Fixed base, double bps) noexcept {
        const double shifted = static_cast<double>(base.raw) * (1.0 + bps / 1e4);
        return Fixed{static_cast<int64_t>(shifted)};
    }

    QuoterConfig cfg_;
};

}  // namespace llt::strat
