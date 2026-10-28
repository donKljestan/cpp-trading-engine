#pragma once

// Microstructure signals.
//
// Everything here is driven by market time from the matching engine, not by local
// arrival time, so a replay produces identical numbers regardless of host or speed.

#include <cmath>
#include <cstdint>

#include "llt/venue/events.hpp"

namespace llt::strat {

inline constexpr double kLn2 = 0.6931471805599453;

// Exponential moving average with time decay.
//
// A plain per-update EWMA weights a burst of twenty quotes in one millisecond the same
// as twenty quotes spread over a minute. Decaying by elapsed time instead makes the
// average mean what it claims to.
class TimeEwma {
public:
    explicit TimeEwma(double half_life_s) noexcept
        : tau_ns_(half_life_s * 1e9 / kLn2) {}

    void update(double x, uint64_t t_ns) noexcept {
        if (!init_) {
            value_   = x;
            last_ns_ = t_ns;
            init_    = true;
            return;
        }
        // Out-of-order or duplicate timestamps carry no new time information.
        if (t_ns <= last_ns_) {
            value_ = x;
            return;
        }

        const double dt    = static_cast<double>(t_ns - last_ns_);
        const double alpha = 1.0 - std::exp(-dt / tau_ns_);
        value_ += alpha * (x - value_);
        last_ns_ = t_ns;
    }

    [[nodiscard]] double value() const noexcept { return value_; }
    [[nodiscard]] bool   ready() const noexcept { return init_; }

private:
    double   tau_ns_;
    double   value_   = 0.0;
    uint64_t last_ns_ = 0;
    bool     init_    = false;
};

// Decaying sum: old contributions fade, new ones count in full.
//
// Trade flow is a quantity of volume, not an average of volumes. Averaging it would
// also make a single print read as a complete imbalance, because an EWMA seeded with
// its first observation starts at that observation.
class DecayingSum {
public:
    explicit DecayingSum(double half_life_s) noexcept
        : tau_ns_(half_life_s * 1e9 / kLn2) {}

    void add(double x, uint64_t t_ns) noexcept {
        if (init_ && t_ns > last_ns_) {
            sum_ *= std::exp(-static_cast<double>(t_ns - last_ns_) / tau_ns_);
            last_ns_ = t_ns;
        } else if (!init_) {
            last_ns_ = t_ns;
        }
        sum_ += x;
        init_ = true;
    }

    [[nodiscard]] double value() const noexcept { return sum_; }

private:
    double   tau_ns_;
    double   sum_     = 0.0;
    uint64_t last_ns_ = 0;
    bool     init_    = false;
};

// At namespace scope rather than nested in Signals: a nested type's default member
// initializers are not usable in a default argument of the enclosing class, because
// the enclosing class is still incomplete at that point.
struct SignalConfig {
    double vol_half_life_s  = 10.0;
    double flow_half_life_s = 5.0;
    size_t obi_depth        = 5;

    // Two quotes in the same millisecond would otherwise divide by almost zero.
    double min_dt_s = 0.001;

    // Below these, a signal reports neutral rather than a confident number derived
    // from almost no observations.
    double   min_trades_for_flow = 5.0;
    uint64_t min_samples_for_vol = 10;
};

class Signals {
public:
    using Config = SignalConfig;

    explicit Signals(SignalConfig cfg = SignalConfig{})
        : cfg_(cfg),
          var_rate_(cfg.vol_half_life_s),
          signed_flow_(cfg.flow_half_life_s),
          abs_flow_(cfg.flow_half_life_s),
          trade_count_(cfg.flow_half_life_s) {}

    void on_book_ticker(const BookTicker& bt) noexcept {
        const Fixed    mid = bt.mid();
        const uint64_t now = bt.time.market_ns();

        if (have_book_ && last_mid_.raw > 0 && now > last_ns_) {
            const double dt_s =
                std::max(cfg_.min_dt_s, static_cast<double>(now - last_ns_) / 1e9);
            const double ret_bps = static_cast<double>(mid.raw - last_mid_.raw) /
                                   static_cast<double>(last_mid_.raw) * 1e4;

            // Variance scales with time, so the quantity worth averaging is a rate.
            var_rate_.update(ret_bps * ret_bps / dt_s, now);
            ++vol_samples_;
        }

        book_      = bt;
        last_mid_  = mid;
        last_ns_   = now;
        have_book_ = true;
    }

    void on_depth(const DepthSnapshot& d) noexcept {
        obi_      = d.imbalance(cfg_.obi_depth);
        have_obi_ = true;
    }

    void on_trade(const Trade& t) noexcept {
        const uint64_t now = t.time.market_ns();
        const double   qty = t.qty.to_double();

        signed_flow_.add((t.aggressor() == Side::Buy) ? qty : -qty, now);
        abs_flow_.add(qty, now);
        trade_count_.add(1.0, now);
    }

    [[nodiscard]] bool ready() const noexcept { return have_book_; }

    [[nodiscard]] const BookTicker& book() const noexcept { return book_; }
    [[nodiscard]] Fixed             mid() const noexcept { return last_mid_; }

    // Size-weighted mid: leans toward the side with less resting size, which is the
    // side the next trade is more likely to move.
    [[nodiscard]] Fixed fair_value() const noexcept { return book_.microprice(); }

    // Resting size imbalance over the configured depth, in [-1, 1].
    [[nodiscard]] double obi() const noexcept { return have_obi_ ? obi_ : 0.0; }

    // Signed trade flow as a fraction of total flow, in [-1, 1].
    [[nodiscard]] double tfi() const noexcept {
        if (trade_count_.value() < cfg_.min_trades_for_flow) return 0.0;

        const double denom = abs_flow_.value();
        if (denom <= 0.0) return 0.0;

        const double r = signed_flow_.value() / denom;
        return (r > 1.0) ? 1.0 : (r < -1.0 ? -1.0 : r);
    }

    // Volatility in basis points per square-root second.
    [[nodiscard]] double vol_bps() const noexcept {
        if (vol_samples_ < cfg_.min_samples_for_vol) return 0.0;
        const double rate = var_rate_.value();
        return (rate > 0.0) ? std::sqrt(rate) : 0.0;
    }

    // How much flow the lookback window currently holds, for callers that want to
    // know whether tfi is speaking from evidence.
    [[nodiscard]] double effective_trades() const noexcept { return trade_count_.value(); }

    [[nodiscard]] uint64_t last_market_ns() const noexcept { return last_ns_; }

private:
    SignalConfig cfg_;

    TimeEwma    var_rate_;
    DecayingSum signed_flow_;
    DecayingSum abs_flow_;
    DecayingSum trade_count_;

    BookTicker book_{};
    Fixed      last_mid_{};
    uint64_t   last_ns_     = 0;
    uint64_t   vol_samples_ = 0;
    double     obi_         = 0.0;

    bool have_book_ = false;
    bool have_obi_  = false;
};

}  // namespace llt::strat
