#pragma once

// Fixed-point decimal arithmetic for prices and quantities.
//
// Every price and size in this system is an int64 scaled by 1e8. Binary floating
// point is never used: 0.1 is not representable in a double, and a price that is
// off by one ULP is a rejected order or a silently wrong tick comparison.

#include <cstddef>
#include <cstdint>
#include <compare>

namespace llt {

inline constexpr int     kFixedDigits = 8;
inline constexpr int64_t kFixedScale  = 100'000'000;

// Largest magnitude representable: ~9.2e10. Ample for any crypto price or size.
struct Fixed {
    int64_t raw = 0;

    [[nodiscard]] static constexpr Fixed from_raw(int64_t r) noexcept { return Fixed{r}; }
    [[nodiscard]] static constexpr Fixed from_int(int64_t v) noexcept { return Fixed{v * kFixedScale}; }

    // Only for config parsing and tests; never call this on a hot path.
    [[nodiscard]] static constexpr Fixed from_double(double v) noexcept {
        return Fixed{static_cast<int64_t>(v * static_cast<double>(kFixedScale))};
    }

    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(raw) / static_cast<double>(kFixedScale);
    }

    [[nodiscard]] constexpr bool is_zero() const noexcept { return raw == 0; }

    constexpr auto operator<=>(const Fixed&) const noexcept = default;
    constexpr bool operator==(const Fixed&) const noexcept  = default;

    constexpr Fixed& operator+=(Fixed o) noexcept { raw += o.raw; return *this; }
    constexpr Fixed& operator-=(Fixed o) noexcept { raw -= o.raw; return *this; }
};

[[nodiscard]] constexpr Fixed operator+(Fixed a, Fixed b) noexcept { return Fixed{a.raw + b.raw}; }
[[nodiscard]] constexpr Fixed operator-(Fixed a, Fixed b) noexcept { return Fixed{a.raw - b.raw}; }
[[nodiscard]] constexpr Fixed operator-(Fixed a) noexcept { return Fixed{-a.raw}; }
[[nodiscard]] constexpr Fixed operator*(Fixed a, int64_t k) noexcept { return Fixed{a.raw * k}; }
[[nodiscard]] constexpr Fixed operator*(int64_t k, Fixed a) noexcept { return Fixed{a.raw * k}; }

// Scaled product, e.g. notional = price * quantity. Uses 128-bit intermediates because
// price.raw * qty.raw overflows int64 at realistic values (1e13 * 1e11 = 1e24).
[[nodiscard]] constexpr Fixed mul(Fixed a, Fixed b) noexcept {
    return Fixed{static_cast<int64_t>((static_cast<__int128_t>(a.raw) * b.raw) / kFixedScale)};
}

// Caller must guarantee b != 0.
[[nodiscard]] constexpr Fixed div(Fixed a, Fixed b) noexcept {
    return Fixed{static_cast<int64_t>((static_cast<__int128_t>(a.raw) * kFixedScale) / b.raw)};
}

// Truncating integer division rounds toward zero, which would round a negative price
// up and break tick alignment below zero. These round toward negative infinity.
[[nodiscard]] constexpr int64_t floor_div(int64_t a, int64_t b) noexcept {
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

[[nodiscard]] constexpr Fixed round_down(Fixed v, Fixed step) noexcept {
    return Fixed{floor_div(v.raw, step.raw) * step.raw};
}

[[nodiscard]] constexpr Fixed round_up(Fixed v, Fixed step) noexcept {
    return Fixed{floor_div(v.raw + step.raw - 1, step.raw) * step.raw};
}

[[nodiscard]] constexpr Fixed round_nearest(Fixed v, Fixed step) noexcept {
    return Fixed{floor_div(v.raw + step.raw / 2, step.raw) * step.raw};
}

[[nodiscard]] constexpr Fixed abs(Fixed v) noexcept { return Fixed{v.raw < 0 ? -v.raw : v.raw}; }
[[nodiscard]] constexpr Fixed min(Fixed a, Fixed b) noexcept { return a.raw < b.raw ? a : b; }
[[nodiscard]] constexpr Fixed max(Fixed a, Fixed b) noexcept { return a.raw > b.raw ? a : b; }

// Parses a plain decimal string as emitted by Binance ("67890.10", "-0.00012340").
// Exponent notation is deliberately unsupported: the exchange never sends it, and
// accepting it would mean a slower parser on the hot path. Fractional digits beyond
// 1e-8 are truncated, never rounded up.
[[nodiscard]] constexpr bool parse_fixed(const char* first, const char* last, Fixed& out) noexcept {
    const char* p   = first;
    bool        neg = false;
    if (p != last && (*p == '-' || *p == '+')) {
        neg = (*p == '-');
        ++p;
    }

    int64_t mant     = 0;
    int     ndigits  = 0;
    bool    any      = false;

    for (; p != last && *p >= '0' && *p <= '9'; ++p) {
        if (ndigits >= 18) return false;
        mant = mant * 10 + (*p - '0');
        ++ndigits;
        any = true;
    }

    int frac = 0;
    if (p != last && *p == '.') {
        ++p;
        for (; p != last && *p >= '0' && *p <= '9'; ++p) {
            any = true;
            if (frac < kFixedDigits) {
                if (ndigits >= 18) return false;
                mant = mant * 10 + (*p - '0');
                ++ndigits;
                ++frac;
            }
        }
    }

    if (!any || p != last) return false;

    for (; frac < kFixedDigits; ++frac) {
        if (ndigits >= 18) return false;
        mant *= 10;
        ++ndigits;
    }

    out.raw = neg ? -mant : mant;
    return true;
}

inline constexpr size_t kFixedMaxChars = 24;

// Writes a decimal representation with trailing fractional zeros trimmed.
// Returns bytes written, or 0 if cap is too small.
[[nodiscard]] constexpr size_t format_fixed(Fixed v, char* buf, size_t cap) noexcept {
    char   tmp[kFixedMaxChars] = {};
    size_t n                   = 0;

    const bool     neg = v.raw < 0;
    const uint64_t mag = neg ? (~static_cast<uint64_t>(v.raw) + 1U) : static_cast<uint64_t>(v.raw);

    uint64_t ip = mag / static_cast<uint64_t>(kFixedScale);
    uint64_t fp = mag % static_cast<uint64_t>(kFixedScale);

    int fd = kFixedDigits;
    while (fd > 0 && fp % 10 == 0) {
        fp /= 10;
        --fd;
    }
    for (int i = 0; i < fd; ++i) {
        tmp[n++] = static_cast<char>('0' + fp % 10);
        fp /= 10;
    }
    if (fd > 0) tmp[n++] = '.';

    if (ip == 0) {
        tmp[n++] = '0';
    } else {
        while (ip != 0) {
            tmp[n++] = static_cast<char>('0' + ip % 10);
            ip /= 10;
        }
    }
    if (neg) tmp[n++] = '-';

    if (n > cap) return 0;
    for (size_t i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    return n;
}

}  // namespace llt
