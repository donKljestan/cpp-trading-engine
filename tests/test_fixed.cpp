#include "harness.hpp"
#include "llt/core/fixed.hpp"

#include <cstring>
#include <string>

using llt::Fixed;

namespace {

Fixed parsed(const char* s) {
    Fixed f{};
    const bool ok = llt::parse_fixed(s, s + std::strlen(s), f);
    LLT_CHECK(ok);
    return f;
}

std::string formatted(Fixed v) {
    char         buf[llt::kFixedMaxChars];
    const size_t n = llt::format_fixed(v, buf, sizeof(buf));
    return std::string(buf, n);
}

}  // namespace

LLT_TEST(fixed_parse_basic) {
    LLT_CHECK_EQ(parsed("0").raw, 0);
    LLT_CHECK_EQ(parsed("1").raw, 100'000'000);
    LLT_CHECK_EQ(parsed("1.0").raw, 100'000'000);
    LLT_CHECK_EQ(parsed("67890.10").raw, 6'789'010'000'000);
    LLT_CHECK_EQ(parsed("0.00012340").raw, 12'340);
    LLT_CHECK_EQ(parsed("-0.5").raw, -50'000'000);
    LLT_CHECK_EQ(parsed(".5").raw, 50'000'000);
}

LLT_TEST(fixed_parse_truncates_excess_precision) {
    // A price must never be rounded up by the parser.
    LLT_CHECK_EQ(parsed("0.123456789").raw, 12'345'678);
    LLT_CHECK_EQ(parsed("0.999999999").raw, 99'999'999);
}

LLT_TEST(fixed_parse_rejects_malformed) {
    Fixed f{};
    const auto bad = [&](const char* s) {
        return !llt::parse_fixed(s, s + std::strlen(s), f);
    };
    LLT_CHECK(bad(""));
    LLT_CHECK(bad("-"));
    LLT_CHECK(bad("abc"));
    LLT_CHECK(bad("1.2.3"));
    LLT_CHECK(bad("1e5"));     // exponent form is intentionally unsupported
    LLT_CHECK(bad("1 "));
    LLT_CHECK(bad("99999999999999999999"));
}

LLT_TEST(fixed_format_roundtrip) {
    LLT_CHECK(formatted(Fixed::from_int(0)) == "0");
    LLT_CHECK(formatted(Fixed::from_int(1)) == "1");
    LLT_CHECK(formatted(parsed("67890.10")) == "67890.1");
    LLT_CHECK(formatted(parsed("0.00012340")) == "0.0001234");
    LLT_CHECK(formatted(parsed("-0.5")) == "-0.5");
    LLT_CHECK(formatted(Fixed::from_raw(1)) == "0.00000001");
}

LLT_TEST(fixed_arithmetic) {
    LLT_CHECK_EQ((parsed("1.5") + parsed("2.25")).raw, parsed("3.75").raw);
    LLT_CHECK_EQ((parsed("1.5") - parsed("2.0")).raw, parsed("-0.5").raw);
    LLT_CHECK_EQ((parsed("1.5") * 4).raw, parsed("6").raw);
}

LLT_TEST(fixed_mul_no_overflow_at_realistic_values) {
    // 100k price * 1000 size would overflow a naive int64 product.
    const Fixed notional = llt::mul(parsed("100000.0"), parsed("1000.0"));
    LLT_CHECK_EQ(notional.raw, parsed("100000000.0").raw);
}

LLT_TEST(fixed_div) {
    LLT_CHECK_EQ(llt::div(parsed("1.0"), parsed("4.0")).raw, parsed("0.25").raw);
    LLT_CHECK_EQ(llt::div(parsed("100.0"), parsed("3.0")).raw, 3'333'333'333);
}

LLT_TEST(fixed_tick_rounding_positive) {
    const Fixed tick = parsed("0.10");
    LLT_CHECK_EQ(llt::round_down(parsed("1.24"), tick).raw, parsed("1.2").raw);
    LLT_CHECK_EQ(llt::round_up(parsed("1.21"), tick).raw, parsed("1.3").raw);
    LLT_CHECK_EQ(llt::round_nearest(parsed("1.24"), tick).raw, parsed("1.2").raw);
    LLT_CHECK_EQ(llt::round_nearest(parsed("1.25"), tick).raw, parsed("1.3").raw);
    // Already aligned values must be left alone.
    LLT_CHECK_EQ(llt::round_down(parsed("1.2"), tick).raw, parsed("1.2").raw);
    LLT_CHECK_EQ(llt::round_up(parsed("1.2"), tick).raw, parsed("1.2").raw);
}

LLT_TEST(fixed_tick_rounding_negative) {
    // Truncating division would round -1.24 up to -1.2; floor semantics must not.
    const Fixed tick = parsed("0.10");
    LLT_CHECK_EQ(llt::round_down(parsed("-1.24"), tick).raw, parsed("-1.3").raw);
    LLT_CHECK_EQ(llt::round_up(parsed("-1.24"), tick).raw, parsed("-1.2").raw);
}

LLT_TEST(fixed_comparison) {
    LLT_CHECK(parsed("1.0") < parsed("1.00000001"));
    LLT_CHECK(parsed("-1.0") < parsed("0"));
    LLT_CHECK(parsed("1.50") == parsed("1.5"));
    LLT_CHECK(llt::max(parsed("1.0"), parsed("2.0")) == parsed("2.0"));
    LLT_CHECK(llt::min(parsed("1.0"), parsed("2.0")) == parsed("1.0"));
    LLT_CHECK(llt::abs(parsed("-2.5")) == parsed("2.5"));
}
