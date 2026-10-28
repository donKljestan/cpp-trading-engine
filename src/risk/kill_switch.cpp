#include "llt/risk/kill_switch.hpp"

#include <unistd.h>

#include <utility>

namespace llt::risk {

KillSwitch::KillSwitch(std::string path, uint64_t poll_ms) noexcept
    : path_(std::move(path)), poll_ns_(poll_ms * uint64_t{1'000'000}) {}

bool KillSwitch::engaged(uint64_t now_ns) noexcept {
    if (latched_) return true;
    if (path_.empty()) return false;

    const bool due = !checked_once_ || now_ns < last_check_ns_ ||
                     (now_ns - last_check_ns_) >= poll_ns_;
    if (!due) return false;

    last_check_ns_ = now_ns;
    checked_once_  = true;
    ++checks_;

    if (::access(path_.c_str(), F_OK) == 0) latched_ = true;
    return latched_;
}

}  // namespace llt::risk
