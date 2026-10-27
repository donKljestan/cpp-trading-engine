#include "harness.hpp"
#include "llt/venue/json_scan.hpp"

#include <string>
#include <vector>

using llt::Fixed;
using llt::json::ObjectScanner;
using llt::json::PairArrayScanner;

namespace {

std::vector<std::pair<std::string, std::string>> scan_all(std::string_view json) {
    std::vector<std::pair<std::string, std::string>> out;
    ObjectScanner                                    sc(json);
    std::string_view                                 k;
    std::string_view                                 v;
    while (sc.next(k, v)) out.emplace_back(std::string(k), std::string(v));
    return out;
}

}  // namespace

LLT_TEST(json_scan_flat_object) {
    const auto kv = scan_all(R"({"a":1,"b":"two","c":true})");
    LLT_CHECK_EQ(kv.size(), 3U);
    LLT_CHECK(kv[0].first == "a" && kv[0].second == "1");
    LLT_CHECK(kv[1].first == "b" && kv[1].second == "\"two\"");
    LLT_CHECK(kv[2].first == "c" && kv[2].second == "true");
}

LLT_TEST(json_scan_returns_nested_values_whole) {
    const auto kv = scan_all(R"({"outer":{"inner":{"deep":1}},"after":2})");
    LLT_CHECK_EQ(kv.size(), 2U);
    LLT_CHECK(kv[0].second == R"({"inner":{"deep":1}})");
    LLT_CHECK(kv[1].second == "2");
}

LLT_TEST(json_scan_handles_arrays_of_objects) {
    const auto kv = scan_all(R"({"list":[{"x":1},{"y":2}],"n":3})");
    LLT_CHECK_EQ(kv.size(), 2U);
    LLT_CHECK(kv[0].second == R"([{"x":1},{"y":2}])");
    LLT_CHECK(kv[1].second == "3");
}

LLT_TEST(json_scan_braces_inside_strings_do_not_confuse_depth) {
    const auto kv = scan_all(R"({"a":"}{][","b":1})");
    LLT_CHECK_EQ(kv.size(), 2U);
    LLT_CHECK(kv[1].first == "b");
    LLT_CHECK(kv[1].second == "1");
}

LLT_TEST(json_scan_handles_escaped_quote) {
    const auto kv = scan_all(R"({"a":"say \"hi\"","b":2})");
    LLT_CHECK_EQ(kv.size(), 2U);
    LLT_CHECK(kv[1].first == "b");
}

LLT_TEST(json_scan_empty_object) {
    LLT_CHECK_EQ(scan_all("{}").size(), 0U);
}

LLT_TEST(json_scan_tolerates_whitespace) {
    const auto kv = scan_all("{ \"a\" : 1 , \"b\" : 2 }");
    LLT_CHECK_EQ(kv.size(), 2U);
    LLT_CHECK(kv[1].first == "b");
}

LLT_TEST(json_parse_helpers) {
    uint64_t u = 0;
    LLT_CHECK(llt::json::parse_uint("12345", u));
    LLT_CHECK_EQ(u, 12345ULL);
    LLT_CHECK(llt::json::parse_uint("\"678\"", u));
    LLT_CHECK_EQ(u, 678ULL);
    LLT_CHECK(!llt::json::parse_uint("12a", u));
    LLT_CHECK(!llt::json::parse_uint("", u));

    bool b = false;
    LLT_CHECK(llt::json::parse_bool("true", b));
    LLT_CHECK(b);
    LLT_CHECK(llt::json::parse_bool("false", b));
    LLT_CHECK(!b);
    LLT_CHECK(!llt::json::parse_bool("maybe", b));

    Fixed f;
    LLT_CHECK(llt::json::parse_decimal("\"80944.60\"", f));
    LLT_CHECK_EQ(f.raw, 8'094'460'000'000);
}

LLT_TEST(json_pair_array_scanner) {
    PairArrayScanner sc(R"([["80944.60","0.742"],["80944.50","1.250"]])");

    Fixed px;
    Fixed qty;

    LLT_CHECK(sc.next(px, qty));
    LLT_CHECK_EQ(px.raw, 8'094'460'000'000);
    LLT_CHECK_EQ(qty.raw, 74'200'000);

    LLT_CHECK(sc.next(px, qty));
    LLT_CHECK_EQ(px.raw, 8'094'450'000'000);
    LLT_CHECK_EQ(qty.raw, 125'000'000);

    LLT_CHECK(!sc.next(px, qty));
}

LLT_TEST(json_pair_array_empty) {
    PairArrayScanner sc("[]");
    Fixed            px;
    Fixed            qty;
    LLT_CHECK(!sc.next(px, qty));
}
