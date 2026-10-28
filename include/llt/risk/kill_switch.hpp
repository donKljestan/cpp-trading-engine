#pragma once

// An operator's veto that needs no signal, no port and no password: a file.
//
// `touch /run/llt/STOP` over SSH stops trading. It works when the process is wedged
// in a way that ignores signals, it leaves a timestamped artefact of who stopped
// what and when, and it survives a restart - the engine will not come back up
// trading while the file is there.

#include <cstdint>
#include <string>

namespace llt::risk {

class KillSwitch {
public:
    explicit KillSwitch(std::string path, uint64_t poll_ms = 250) noexcept;

    // Throttled: the trading loop may ask every tick, but the filesystem is only
    // consulted on the poll interval. Once engaged it stays engaged even if the file
    // is removed, so deleting it cannot silently resume trading.
    [[nodiscard]] bool engaged(uint64_t now_ns) noexcept;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] bool               latched() const noexcept { return latched_; }
    [[nodiscard]] uint64_t           checks() const noexcept { return checks_; }

private:
    std::string path_;
    uint64_t    poll_ns_;
    uint64_t    last_check_ns_ = 0;
    uint64_t    checks_        = 0;
    bool        checked_once_  = false;
    bool        latched_       = false;
};

}  // namespace llt::risk
