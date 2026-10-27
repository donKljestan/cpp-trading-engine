#include "harness.hpp"
#include "llt/venue/binance/bn_parse.hpp"

using llt::BookTicker;
using llt::DepthSnapshot;
using llt::Fixed;
using llt::Side;
using llt::Trade;

namespace {

// Captured verbatim from fstream.binance.com on 2026-10-27.
constexpr const char* kCombinedBookTicker =
    R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":11941249316564,)"
    R"("s":"BTCUSDT","ps":"BTCUSDT","b":"80944.60","B":"0.742","a":"80944.70",)"
    R"("A":"2.544","T":1793138014442,"E":1793138014442,"st":1}})";

constexpr const char* kDepth =
    R"({"e":"depthUpdate","E":1571889248277,"T":1571889248276,"s":"BTCUSDT",)"
    R"("U":390497796,"u":390497878,"pu":390497794,)"
    R"("b":[["7403.89","0.002"],["7403.90","3.906"],["7404.00","1.000"]],)"
    R"("a":[["7405.96","3.340"],["7406.63","4.525"]]})";

constexpr const char* kAggTrade =
    R"({"e":"aggTrade","E":1591261134288,"a":424951,"s":"BTCUSDT","p":"9643.5",)"
    R"("q":"2","f":606073,"l":606073,"T":1591261134199,"m":false})";

// Captured verbatim from fstream.binance.com on 2026-10-27. The @aggTrade stream is
// silent on this venue now; @trade is what actually arrives.
constexpr const char* kTrade =
    R"({"e":"trade","E":1793140093437,"T":1793140093437,"s":"BTCUSDT",)"
    R"("t":8209756674,"p":"80887.70","q":"0.001","X":"MARKET","m":true,"st":1})";

}  // namespace

LLT_TEST(bn_unwrap_combined_stream) {
    std::string_view stream;
    std::string_view data;
    LLT_CHECK(llt::binance::unwrap_combined(kCombinedBookTicker, stream, data));
    LLT_CHECK(stream == "btcusdt@bookTicker");
    LLT_CHECK(data.front() == '{' && data.back() == '}');
    LLT_CHECK(data.find("\"bookTicker\"") != std::string_view::npos);
}

LLT_TEST(bn_unwrap_rejects_raw_payload) {
    std::string_view stream;
    std::string_view data;
    LLT_CHECK(!llt::binance::unwrap_combined(kAggTrade, stream, data));
}

LLT_TEST(bn_parse_book_ticker) {
    std::string_view stream;
    std::string_view data;
    LLT_CHECK(llt::binance::unwrap_combined(kCombinedBookTicker, stream, data));

    BookTicker bt{};
    LLT_CHECK(llt::binance::parse_book_ticker(data, 4242, bt));

    LLT_CHECK(bt.symbol.view() == "BTCUSDT");
    LLT_CHECK_EQ(bt.update_id, 11941249316564ULL);
    LLT_CHECK_EQ(bt.bid_px.raw, 8'094'460'000'000);
    LLT_CHECK_EQ(bt.bid_qty.raw, 74'200'000);
    LLT_CHECK_EQ(bt.ask_px.raw, 8'094'470'000'000);
    LLT_CHECK_EQ(bt.ask_qty.raw, 254'400'000);
    LLT_CHECK_EQ(bt.time.exchange_ms, 1793138014442ULL);
    LLT_CHECK_EQ(bt.time.transact_ms, 1793138014442ULL);
    LLT_CHECK_EQ(bt.time.recv_tsc, 4242ULL);
}

LLT_TEST(bn_book_ticker_derived_values) {
    std::string_view stream;
    std::string_view data;
    LLT_CHECK(llt::binance::unwrap_combined(kCombinedBookTicker, stream, data));

    BookTicker bt{};
    LLT_CHECK(llt::binance::parse_book_ticker(data, 0, bt));

    // One tick on BTCUSDT futures.
    LLT_CHECK_EQ(bt.spread().raw, 10'000'000);
    LLT_CHECK_EQ(bt.mid().raw, 8'094'465'000'000);

    // More size offered than bid, so the microprice must sit below the mid.
    const Fixed micro = bt.microprice();
    LLT_CHECK(micro > bt.bid_px);
    LLT_CHECK(micro < bt.ask_px);
    LLT_CHECK(micro < bt.mid());
}

LLT_TEST(bn_microprice_is_symmetric_for_equal_sizes) {
    const char* json =
        R"({"s":"X","b":"100.00","B":"5","a":"101.00","A":"5","u":1,"E":1,"T":1})";

    BookTicker bt{};
    LLT_CHECK(llt::binance::parse_book_ticker(json, 0, bt));
    LLT_CHECK_EQ(bt.microprice().raw, bt.mid().raw);
}

LLT_TEST(bn_parse_depth) {
    DepthSnapshot d{};
    LLT_CHECK(llt::binance::parse_depth(kDepth, 77, d));

    LLT_CHECK(d.symbol.view() == "BTCUSDT");
    LLT_CHECK_EQ(d.first_update_id, 390497796ULL);
    LLT_CHECK_EQ(d.final_update_id, 390497878ULL);
    LLT_CHECK_EQ(d.prev_final_id, 390497794ULL);
    LLT_CHECK_EQ(d.time.recv_tsc, 77ULL);

    LLT_CHECK_EQ(static_cast<int>(d.n_bids), 3);
    LLT_CHECK_EQ(static_cast<int>(d.n_asks), 2);
    LLT_CHECK_EQ(d.bids[0].px.raw, 740'389'000'000);
    LLT_CHECK_EQ(d.bids[0].qty.raw, 200'000);
    LLT_CHECK_EQ(d.asks[1].px.raw, 740'663'000'000);
    LLT_CHECK_EQ(d.asks[1].qty.raw, 452'500'000);
}

LLT_TEST(bn_depth_imbalance) {
    DepthSnapshot d{};
    LLT_CHECK(llt::binance::parse_depth(kDepth, 0, d));

    // bids 0.002 + 3.906 = 3.908 against asks 3.340 + 4.525 = 7.865
    const double imb = d.imbalance(2);
    LLT_CHECK(imb < 0.0);
    LLT_CHECK(imb > -1.0);

    // Top level alone: 0.002 bid against 3.340 ask, heavily offered.
    LLT_CHECK(d.imbalance(1) < -0.9);
}

LLT_TEST(bn_depth_imbalance_is_zero_on_empty_book) {
    DepthSnapshot d{};
    LLT_CHECK_EQ(d.imbalance(5), 0.0);
}

LLT_TEST(bn_parse_agg_trade) {
    Trade t{};
    LLT_CHECK(llt::binance::parse_trade(kAggTrade, 9, t));

    LLT_CHECK(t.symbol.view() == "BTCUSDT");
    LLT_CHECK_EQ(t.id, 424951ULL);
    LLT_CHECK_EQ(t.px.raw, 964'350'000'000);
    LLT_CHECK_EQ(t.qty.raw, 200'000'000);
    LLT_CHECK(!t.buyer_is_maker);
    LLT_CHECK(t.aggressor() == Side::Buy);
    LLT_CHECK_EQ(t.time.transact_ms, 1591261134199ULL);
}

LLT_TEST(bn_parse_trade) {
    Trade t{};
    LLT_CHECK(llt::binance::parse_trade(kTrade, 11, t));

    LLT_CHECK(t.symbol.view() == "BTCUSDT");
    LLT_CHECK_EQ(t.id, 8209756674ULL);
    LLT_CHECK_EQ(t.px.raw, 8'088'770'000'000);
    LLT_CHECK_EQ(t.qty.raw, 100'000);
    LLT_CHECK(t.buyer_is_maker);
    LLT_CHECK(t.aggressor() == Side::Sell);
    LLT_CHECK_EQ(t.time.recv_tsc, 11ULL);
}

LLT_TEST(bn_agg_trade_maker_flag_flips_aggressor) {
    const char* json =
        R"({"e":"aggTrade","E":1,"a":2,"s":"X","p":"1","q":"1","T":3,"m":true})";

    Trade t{};
    LLT_CHECK(llt::binance::parse_trade(json, 0, t));
    LLT_CHECK(t.buyer_is_maker);
    LLT_CHECK(t.aggressor() == Side::Sell);
}

LLT_TEST(bn_parsers_reject_incomplete_payloads) {
    BookTicker bt{};
    // Missing the ask side entirely.
    LLT_CHECK(!llt::binance::parse_book_ticker(R"({"s":"X","b":"1","B":"2"})", 0, bt));

    Trade t{};
    LLT_CHECK(!llt::binance::parse_trade(R"({"s":"X","p":"1"})", 0, t));

    DepthSnapshot d{};
    LLT_CHECK(!llt::binance::parse_depth(R"({"s":"X","b":[]})", 0, d));
}

LLT_TEST(bn_depth_caps_at_max_levels) {
    std::string json = R"({"s":"X","U":1,"u":2,"pu":0,"b":[)";
    for (int i = 0; i < 40; ++i) {
        if (i != 0) json += ',';
        json += R"(["1.0","1.0"])";
    }
    json += R"(],"a":[["2.0","1.0"]]})";

    DepthSnapshot d{};
    LLT_CHECK(llt::binance::parse_depth(json, 0, d));
    LLT_CHECK_EQ(static_cast<size_t>(d.n_bids), DepthSnapshot::kMaxLevels);
}
