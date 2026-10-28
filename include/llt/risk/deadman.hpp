#pragma once

// Dead-man switch timing.
//
// Every other check in this engine assumes the engine is running. This one is the
// only protection that survives the engine dying: we ask the venue to cancel all our
// orders unless we keep saying we are alive. Binance USDⓈ-M exposes it as
// POST /fapi/v1/countdownCancelAll with a symbol and a countdownTime in ms — a
// timer the venue holds and we refresh. If the process is killed, the network is
// severed or the host is terminated, the venue cancels for us.
//
// This header owns only the timing decision; the caller owns the transport.

#include <cstdint>

namespace llt::risk {

class DeadMan {
public:
    // The refresh interval is forced to at most a third of the countdown, so two
    // refreshes in a row must be lost before the venue acts. A single late tick
    // caused by a GC pause, a reconnect or a scheduling hiccup should not flatten a
    // healthy book.
    DeadMan(uint64_t countdown_ms, uint64_t refresh_ms) noexcept
        : countdown_ms_(countdown_ms == 0 ? 1 : countdown_ms) {
        const uint64_t cap = countdown_ms_ / 3;
        const uint64_t max_refresh = cap == 0 ? 1 : cap;
        refresh_ms_ = (refresh_ms == 0 || refresh_ms > max_refresh) ? max_refresh : refresh_ms;
    }

    [[nodiscard]] bool due(uint64_t now_ns) const noexcept {
        if (!armed_) return true;
        if (now_ns <= last_sent_ns_) return false;
        return (now_ns - last_sent_ns_) >= refresh_ms_ * uint64_t{1'000'000};
    }

    void sent(uint64_t now_ns) noexcept {
        last_sent_ns_ = now_ns;
        armed_        = true;
        ++refreshes_;
    }

    // True when the venue's timer has probably already fired, so any order we think
    // is resting may no longer exist. Before it has ever been armed this is also
    // true: no protection is in place yet.
    [[nodiscard]] bool lapsed(uint64_t now_ns) const noexcept {
        if (!armed_) return true;
        if (now_ns <= last_sent_ns_) return false;
        return (now_ns - last_sent_ns_) > countdown_ms_ * uint64_t{1'000'000};
    }

    void disarm() noexcept { armed_ = false; }

    [[nodiscard]] bool     armed() const noexcept { return armed_; }
    [[nodiscard]] uint64_t countdown_ms() const noexcept { return countdown_ms_; }
    [[nodiscard]] uint64_t refresh_ms() const noexcept { return refresh_ms_; }
    [[nodiscard]] uint64_t refreshes() const noexcept { return refreshes_; }

private:
    uint64_t countdown_ms_;
    uint64_t refresh_ms_   = 1;
    uint64_t last_sent_ns_ = 0;
    uint64_t refreshes_    = 0;
    bool     armed_        = false;
};

}  // namespace llt::risk
