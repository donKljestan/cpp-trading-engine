#include "harness.hpp"
#include "llt/venue/binance/bn_rest.hpp"

#include <string>
#include <vector>

using llt::Instrument;

namespace {

// Shape taken from a real /fapi/v1/exchangeInfo response, trimmed to the fields the
// parser cares about plus enough noise to prove it skips what it should.
constexpr const char* kExchangeInfo = R"({
  "timezone":"UTC",
  "serverTime":1793138014442,
  "rateLimits":[{"rateLimitType":"REQUEST_WEIGHT","interval":"MINUTE","limit":2400}],
  "exchangeFilters":[],
  "assets":[{"asset":"USDT","marginAvailable":true}],
  "symbols":[
    {"symbol":"BTCUSDT","pair":"BTCUSDT","contractType":"PERPETUAL","status":"TRADING",
     "baseAsset":"BTC","quoteAsset":"USDT","pricePrecision":2,"quantityPrecision":3,
     "filters":[
       {"minPrice":"556.80","maxPrice":"4529764","filterType":"PRICE_FILTER","tickSize":"0.10"},
       {"stepSize":"0.001","filterType":"LOT_SIZE","maxQty":"1000","minQty":"0.001"},
       {"filterType":"MIN_NOTIONAL","notional":"100"}],
     "orderTypes":["LIMIT","MARKET"],"timeInForce":["GTC","IOC","FOK","GTX"]},

    {"symbol":"BTCUSDT_250328","pair":"BTCUSDT","contractType":"CURRENT_QUARTER",
     "status":"TRADING","quoteAsset":"USDT",
     "filters":[{"filterType":"PRICE_FILTER","tickSize":"0.10"},
                {"filterType":"LOT_SIZE","stepSize":"0.001","minQty":"0.001"},
                {"filterType":"MIN_NOTIONAL","notional":"100"}]},

    {"symbol":"ETHBTC","contractType":"PERPETUAL","status":"TRADING","quoteAsset":"BTC",
     "filters":[{"filterType":"PRICE_FILTER","tickSize":"0.00001"},
                {"filterType":"LOT_SIZE","stepSize":"0.001","minQty":"0.001"},
                {"filterType":"MIN_NOTIONAL","notional":"0.001"}]},

    {"symbol":"DEADUSDT","contractType":"PERPETUAL","status":"SETTLING","quoteAsset":"USDT",
     "filters":[{"filterType":"PRICE_FILTER","tickSize":"0.0001"},
                {"filterType":"LOT_SIZE","stepSize":"1","minQty":"1"},
                {"filterType":"MIN_NOTIONAL","notional":"5"}]},

    {"symbol":"SMALLUSDT","contractType":"PERPETUAL","status":"TRADING","quoteAsset":"USDT",
     "filters":[{"filterType":"PRICE_FILTER","tickSize":"0.0001"},
                {"filterType":"LOT_SIZE","stepSize":"1","minQty":"1"},
                {"filterType":"MIN_NOTIONAL","notional":"5"}]}
  ]})";

const Instrument* find(const std::vector<Instrument>& v, std::string_view name) {
    for (const auto& i : v) {
        if (i.symbol.view() == name) return &i;
    }
    return nullptr;
}

}  // namespace

LLT_TEST(exchange_info_parses_perpetual_filters) {
    std::vector<Instrument> out;
    LLT_CHECK(llt::binance::parse_exchange_info(kExchangeInfo, out));

    const Instrument* btc = find(out, "BTCUSDT");
    LLT_CHECK(btc != nullptr);
    if (btc == nullptr) return;

    LLT_CHECK_EQ(btc->tick_size.raw, 10'000'000);      // 0.10
    LLT_CHECK_EQ(btc->step_size.raw, 100'000);         // 0.001
    LLT_CHECK_EQ(btc->min_qty.raw, 100'000);           // 0.001
    LLT_CHECK_EQ(btc->min_notional.raw, 10'000'000'000);  // 100
    LLT_CHECK(btc->tradable);
}

LLT_TEST(exchange_info_skips_dated_futures) {
    std::vector<Instrument> out;
    LLT_CHECK(llt::binance::parse_exchange_info(kExchangeInfo, out));
    // Quarterlies settle and disappear; quoting them is a different business.
    LLT_CHECK(find(out, "BTCUSDT_250328") == nullptr);
}

LLT_TEST(exchange_info_skips_non_usdt_quote) {
    std::vector<Instrument> out;
    LLT_CHECK(llt::binance::parse_exchange_info(kExchangeInfo, out));
    LLT_CHECK(find(out, "ETHBTC") == nullptr);
}

LLT_TEST(exchange_info_keeps_halted_symbols_but_marks_them) {
    std::vector<Instrument> out;
    LLT_CHECK(llt::binance::parse_exchange_info(kExchangeInfo, out));

    const Instrument* dead = find(out, "DEADUSDT");
    LLT_CHECK(dead != nullptr);
    if (dead != nullptr) LLT_CHECK(!dead->tradable);
}

LLT_TEST(exchange_info_counts) {
    std::vector<Instrument> out;
    LLT_CHECK(llt::binance::parse_exchange_info(kExchangeInfo, out));
    LLT_CHECK_EQ(out.size(), 3U);  // BTCUSDT, DEADUSDT, SMALLUSDT
}

LLT_TEST(exchange_info_small_symbol_fits_a_tiny_account) {
    std::vector<Instrument> out;
    LLT_CHECK(llt::binance::parse_exchange_info(kExchangeInfo, out));

    const Instrument* small = find(out, "SMALLUSDT");
    LLT_CHECK(small != nullptr);
    if (small == nullptr) return;

    // Two-sided quoting needs twice the minimum notional to fit the account.
    const llt::Fixed budget = llt::Fixed::from_int(20);
    LLT_CHECK(small->min_notional * 2 <= budget);

    const Instrument* btc = find(out, "BTCUSDT");
    LLT_CHECK(btc != nullptr);
    if (btc != nullptr) LLT_CHECK(btc->min_notional * 2 > budget);
}

LLT_TEST(exchange_info_rejects_garbage) {
    std::vector<Instrument> out;
    LLT_CHECK(!llt::binance::parse_exchange_info(R"({"nope":1})", out));
    LLT_CHECK(!llt::binance::parse_exchange_info(R"({"symbols":[]})", out));
}
