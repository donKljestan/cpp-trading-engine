#pragma once

// Venue-neutral trading types. Nothing here mentions Binance: the Deribit adapter,
// if it ever lands, produces the same types from a different wire format.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "llt/core/fixed.hpp"

namespace llt {

enum class Side : uint8_t { Buy, Sell };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Fixed-capacity instrument name. Copyable into hot-path messages without allocating,
// which a std::string would not be.
class Symbol {
public:
    static constexpr size_t kCapacity = 23;

    constexpr Symbol() noexcept = default;

    explicit Symbol(std::string_view s) noexcept {
        len_ = static_cast<uint8_t>(s.size() < kCapacity ? s.size() : kCapacity);
        std::memcpy(data_, s.data(), len_);
        data_[len_] = '\0';
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(data_, len_);
    }

    [[nodiscard]] const char* c_str() const noexcept { return data_; }
    [[nodiscard]] bool        empty() const noexcept { return len_ == 0; }

    [[nodiscard]] bool operator==(const Symbol& o) const noexcept {
        return len_ == o.len_ && std::memcmp(data_, o.data_, len_) == 0;
    }

private:
    char    data_[kCapacity + 1] = {};
    uint8_t len_                 = 0;
};

// Exchange trading rules for one instrument: everything needed to turn a desired price
// and size into an order the exchange will not reject.
struct Instrument {
    Symbol symbol;
    Fixed  tick_size;     // PRICE_FILTER.tickSize
    Fixed  step_size;     // LOT_SIZE.stepSize
    Fixed  min_qty;       // LOT_SIZE.minQty
    Fixed  min_notional;  // MIN_NOTIONAL.notional
    bool   tradable = false;
};

}  // namespace llt
