#pragma once

// Rate limiting, applied before sending.
//
// The venue answers an over-limit request with a rejection and, repeated, with a ban.
// Counting on our side costs a comparison; finding out from the exchange costs the
// connection. Three limits run at once and an order has to satisfy all of them.

#include <cstdint>

namespace llt::exec {

class TokenBucket {
public:
    // `capacity` tokens refill over `window_s`.
    constexpr TokenBucket(double capacity, double window_s) noexcept
        : capacity_(capacity), per_ns_(capacity / (window_s * 1e9)), tokens_(capacity) {}

    [[nodiscard]] bool try_take(double n, uint64_t now_ns) noexcept {
        refill(now_ns);
        if (tokens_ < n) return false;
        tokens_ -= n;
        return true;
    }

    [[nodiscard]] double available(uint64_t now_ns) noexcept {
        refill(now_ns);
        return tokens_;
    }

private:
    void refill(uint64_t now_ns) noexcept {
        // A separate flag rather than last_ns_ == 0: zero is a perfectly good
        // timestamp, and treating it as "never used" means the bucket never refills
        // for a caller whose clock starts there.
        if (!started_) {
            started_ = true;
            last_ns_ = now_ns;
            return;
        }
        if (now_ns <= last_ns_) return;

        tokens_ += static_cast<double>(now_ns - last_ns_) * per_ns_;
        if (tokens_ > capacity_) tokens_ = capacity_;
        last_ns_ = now_ns;
    }

    double   capacity_;
    double   per_ns_;
    double   tokens_;
    uint64_t last_ns_ = 0;
    bool     started_ = false;
};

// Binance USDⓈ-M futures, as of 2026-10: 2400 request weight per minute, 300 orders
// per 10 seconds, 1200 orders per minute.
class RateLimiter {
public:
    [[nodiscard]] bool allow_request(double weight, uint64_t now_ns) noexcept {
        if (!weight_.try_take(weight, now_ns)) {
            ++rejected_;
            return false;
        }
        return true;
    }

    // All three budgets must have room, and none is spent unless all do.
    [[nodiscard]] bool allow_order(double weight, uint64_t now_ns) noexcept {
        if (weight_.available(now_ns) < weight || orders_10s_.available(now_ns) < 1.0 ||
            orders_1m_.available(now_ns) < 1.0) {
            ++rejected_;
            return false;
        }
        static_cast<void>(weight_.try_take(weight, now_ns));
        static_cast<void>(orders_10s_.try_take(1.0, now_ns));
        static_cast<void>(orders_1m_.try_take(1.0, now_ns));
        return true;
    }

    [[nodiscard]] uint64_t rejected() const noexcept { return rejected_; }

private:
    TokenBucket weight_{2400.0, 60.0};
    TokenBucket orders_10s_{300.0, 10.0};
    TokenBucket orders_1m_{1200.0, 60.0};
    uint64_t    rejected_ = 0;
};

}  // namespace llt::exec
