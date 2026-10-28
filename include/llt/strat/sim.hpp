#pragma once

// Fill simulator for replayed market data.
//
// A backtest is only worth the honesty of its fill model, so the assumptions are
// written down here rather than buried:
//
//   * Our order never moves the market. At $5 of notional this is close to true.
//   * Queue position improves when trades consume the orders ahead of us, and when
//     those orders are cancelled. Cancellations are not observable in this feed, so
//     they are modelled as exponential decay at `queue_decay_per_s`. Left at zero the
//     model fills LESS often than reality; the parameter exists so that assumption
//     can be tested instead of believed.
//   * Price levels are taken from depth snapshots 100 ms apart, so a queue estimate
//     can be that stale.
//   * Anything resting below the deepest visible level is treated as unfillable.
//   * Latency is one fixed number, not a distribution.
//   * Orders are assumed to join the back of the queue at their price, which is
//     correct for a new order and the reason a requote is expensive.

#include <cmath>
#include <cstdint>
#include <deque>

#include "llt/strat/inventory.hpp"
#include "llt/strat/quoter.hpp"
#include "llt/strat/signals.hpp"
#include "llt/venue/events.hpp"
#include "llt/venue/types.hpp"

namespace llt::strat {

struct SimConfig {
    Instrument   instrument;
    QuoterConfig quoter;
    SignalConfig signals;

    double maker_fee_bps = 2.0;  // per side

    // Fraction of the queue ahead of us that is cancelled per second. Zero reproduces
    // the trades-only model; 1.0 means roughly two thirds of it leaves each second.
    double queue_decay_per_s = 0.0;

    // One way. A decision taken now reaches the matching engine this much later.
    uint64_t latency_ns = 2'000'000;
};

struct SimStats {
    uint64_t fills     = 0;
    uint64_t bid_fills = 0;
    uint64_t ask_fills = 0;
    uint64_t requotes  = 0;

    Fixed volume{};
    Fixed max_abs_position{};

    // Mid-price drift after a fill, signed so that negative means the market moved
    // against us: the signature of being picked off.
    double adverse_100ms_bps = 0.0;
    double adverse_1s_bps    = 0.0;
    uint64_t adverse_100ms_n = 0;
    uint64_t adverse_1s_n    = 0;

    uint64_t quoted_ns = 0;
    uint64_t total_ns  = 0;

    // Why fills did or did not happen. A zero-fill run is only informative if the
    // reason is visible.
    uint64_t trades_reaching_quote = 0;
    uint64_t blocked_by_queue      = 0;
    uint64_t quote_suppressed      = 0;
    double   last_queue_ahead      = 0.0;
};

class Simulator {
public:
    explicit Simulator(SimConfig cfg)
        : cfg_(cfg), signals_(cfg.signals), quoter_(cfg.quoter) {}

    void on_book_ticker(const BookTicker& bt) noexcept {
        signals_.on_book_ticker(bt);
        const uint64_t now = bt.time.market_ns();

        advance_time(now);
        resolve_marks(now, bt.mid());
        apply_pending(now);
        maybe_requote(now);
    }

    void on_depth(const DepthSnapshot& d) noexcept {
        signals_.on_depth(d);
        depth_      = d;
        have_depth_ = true;
        apply_pending(d.time.market_ns());
        maybe_requote(d.time.market_ns());
    }

    void on_trade(const Trade& t) noexcept {
        signals_.on_trade(t);
        const uint64_t now = t.time.market_ns();

        advance_time(now);
        apply_pending(now);

        // An aggressive sell walks the bids; an aggressive buy walks the offers.
        if (t.aggressor() == Side::Sell && live_.bid_active &&
            t.px.raw <= live_.bid_px.raw) {
            consume(Side::Buy, t.qty, now);
        }
        if (t.aggressor() == Side::Buy && live_.ask_active &&
            t.px.raw >= live_.ask_px.raw) {
            consume(Side::Sell, t.qty, now);
        }

        maybe_requote(now);
    }

    [[nodiscard]] const SimStats&  stats() const noexcept { return stats_; }
    [[nodiscard]] const Inventory& inventory() const noexcept { return inv_; }
    [[nodiscard]] const Signals&   signals() const noexcept { return signals_; }

    [[nodiscard]] Fixed mark() const noexcept { return signals_.mid(); }

private:
    struct PendingMark {
        uint64_t at_ns;
        Fixed    px;
        Side     side;
        bool     is_one_second;
    };

    void advance_time(uint64_t now) noexcept {
        if (last_ns_ != 0 && now > last_ns_) {
            const uint64_t dt = now - last_ns_;
            stats_.total_ns += dt;
            if (live_.bid_active || live_.ask_active) stats_.quoted_ns += dt;
        }
        if (now > last_ns_) last_ns_ = now;
    }

    // Size resting ahead of us at a price we are about to join.
    [[nodiscard]] Fixed queue_ahead(Side side, Fixed px) const noexcept {
        if (!have_depth_) return Fixed{kUnfillable};

        const PriceLevel* levels = (side == Side::Buy) ? depth_.bids : depth_.asks;
        const size_t      n      = (side == Side::Buy) ? depth_.n_bids : depth_.n_asks;
        if (n == 0) return Fixed{kUnfillable};

        // Improving on the touch means an empty queue in front.
        const bool improves = (side == Side::Buy) ? px.raw > levels[0].px.raw
                                                  : px.raw < levels[0].px.raw;
        if (improves) return Fixed{};

        for (size_t i = 0; i < n; ++i) {
            if (levels[i].px.raw == px.raw) return levels[i].qty;
        }
        // Deeper than anything we can see.
        return Fixed{kUnfillable};
    }

    // Orders ahead of us leave by being cancelled, not only by trading. Applied lazily
    // at the moment the queue is read, which is the only moment it matters.
    void decay_queue(Side side, uint64_t now) noexcept {
        Fixed&    ahead = (side == Side::Buy) ? bid_ahead_ : ask_ahead_;
        uint64_t& since = (side == Side::Buy) ? bid_queue_ns_ : ask_queue_ns_;

        if (cfg_.queue_decay_per_s <= 0.0 || ahead.raw <= 0 || since == 0 || now <= since) {
            since = now;
            return;
        }
        const double dt_s = static_cast<double>(now - since) / 1e9;
        since             = now;
        const double survive = std::exp(-cfg_.queue_decay_per_s * dt_s);
        ahead.raw = static_cast<int64_t>(static_cast<double>(ahead.raw) * survive);
    }

    void consume(Side side, Fixed incoming, uint64_t now) noexcept {
        decay_queue(side, now);

        Fixed& ahead     = (side == Side::Buy) ? bid_ahead_ : ask_ahead_;
        Fixed& remaining = (side == Side::Buy) ? bid_left_ : ask_left_;
        const Fixed px   = (side == Side::Buy) ? live_.bid_px : live_.ask_px;

        ++stats_.trades_reaching_quote;
        stats_.last_queue_ahead = ahead.to_double();

        int64_t left = incoming.raw;

        if (ahead.raw > 0) {
            const int64_t eaten = (ahead.raw < left) ? ahead.raw : left;
            ahead.raw -= eaten;
            left -= eaten;
        }
        if (left <= 0 || remaining.raw <= 0) {
            ++stats_.blocked_by_queue;
            return;
        }

        const int64_t qty = (left < remaining.raw) ? left : remaining.raw;
        remaining.raw -= qty;
        record_fill(side, px, Fixed{qty}, now);
    }

    void record_fill(Side side, Fixed px, Fixed qty, uint64_t now) noexcept {
        const Fixed notional = mul(px, qty);
        const Fixed fee      = Fixed{static_cast<int64_t>(
            static_cast<double>(notional.raw) * cfg_.maker_fee_bps / 1e4)};

        inv_.on_fill(side, px, qty, fee);

        ++stats_.fills;
        if (side == Side::Buy) {
            ++stats_.bid_fills;
        } else {
            ++stats_.ask_fills;
        }
        stats_.volume += qty;

        const Fixed abs_pos = abs(inv_.position());
        if (abs_pos > stats_.max_abs_position) stats_.max_abs_position = abs_pos;

        marks_.push_back({now + 100'000'000, px, side, false});
        marks_.push_back({now + 1'000'000'000, px, side, true});
    }

    // How far the mid travelled after each fill, in our favour or against it.
    void resolve_marks(uint64_t now, Fixed mid) noexcept {
        while (!marks_.empty() && marks_.front().at_ns <= now) {
            const PendingMark m = marks_.front();
            marks_.pop_front();

            if (m.px.raw <= 0) continue;
            const double drift_bps = static_cast<double>(mid.raw - m.px.raw) /
                                     static_cast<double>(m.px.raw) * 1e4;
            const double signed_bps = (m.side == Side::Buy) ? drift_bps : -drift_bps;

            if (m.is_one_second) {
                stats_.adverse_1s_bps += signed_bps;
                ++stats_.adverse_1s_n;
            } else {
                stats_.adverse_100ms_bps += signed_bps;
                ++stats_.adverse_100ms_n;
            }
        }
    }

    void apply_pending(uint64_t now) noexcept {
        if (!has_pending_ || now < pending_at_ns_) return;

        // Keeping the same price keeps the queue slot; changing it goes to the back.
        if (!live_.bid_active || pending_.bid_px.raw != live_.bid_px.raw) {
            bid_ahead_    = queue_ahead(Side::Buy, pending_.bid_px);
            bid_left_     = pending_.qty;
            bid_queue_ns_ = now;
        }
        if (!live_.ask_active || pending_.ask_px.raw != live_.ask_px.raw) {
            ask_ahead_    = queue_ahead(Side::Sell, pending_.ask_px);
            ask_left_     = pending_.qty;
            ask_queue_ns_ = now;
        }
        if (!pending_.bid_active) bid_left_ = Fixed{};
        if (!pending_.ask_active) ask_left_ = Fixed{};

        live_        = pending_;
        has_pending_ = false;
        last_change_ns_ = now;
    }

    void maybe_requote(uint64_t now) noexcept {
        const Quote desired = quoter_.compute(signals_, inv_, cfg_.instrument);
        if (!desired.any()) ++stats_.quote_suppressed;

        // A fully consumed side is no longer resting, whatever the quote object says.
        Quote effective = live_;
        if (bid_left_.raw <= 0) effective.bid_active = false;
        if (ask_left_.raw <= 0) effective.ask_active = false;

        if (!quoter_.should_replace(effective, desired, cfg_.instrument.tick_size, now,
                                    last_change_ns_)) {
            return;
        }

        pending_      = desired;
        pending_at_ns_ = now + cfg_.latency_ns;
        has_pending_  = true;
        ++stats_.requotes;
    }

    static constexpr int64_t kUnfillable = 1'000'000'000'000'000;

    SimConfig cfg_;
    Signals   signals_;
    Quoter    quoter_;
    Inventory inv_;
    SimStats  stats_;

    DepthSnapshot depth_{};
    bool          have_depth_ = false;

    Quote    live_{};
    Quote    pending_{};
    bool     has_pending_   = false;
    uint64_t pending_at_ns_ = 0;

    Fixed bid_ahead_{};
    Fixed ask_ahead_{};
    Fixed bid_left_{};
    Fixed ask_left_{};
    uint64_t bid_queue_ns_ = 0;
    uint64_t ask_queue_ns_ = 0;

    uint64_t last_ns_        = 0;
    uint64_t last_change_ns_ = 0;

    std::deque<PendingMark> marks_;
};

}  // namespace llt::strat
