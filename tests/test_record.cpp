#include "harness.hpp"
#include "llt/core/clock.hpp"
#include "llt/md/record.hpp"
#include "llt/venue/binance/bn_feed.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr const char* kPath = "/tmp/llt_record_roundtrip.mdlog";

const char* const kPayloads[] = {
    R"({"e":"bookTicker","u":1,"s":"BTCUSDT","b":"100.00","B":"1","a":"100.10","A":"2","T":5,"E":5})",
    R"({"e":"trade","E":6,"T":6,"s":"BTCUSDT","t":2,"p":"100.05","q":"0.5","m":false})",
    R"({"e":"depthUpdate","E":7,"T":7,"s":"BTCUSDT","U":1,"u":2,"pu":0,"b":[["99.9","3"]],"a":[["100.2","4"]]})",
};

struct Counter {
    uint64_t books  = 0;
    uint64_t trades = 0;
    uint64_t depths = 0;
    double   last_spread_bps = 0.0;

    void on_book_ticker(const llt::BookTicker& bt) {
        ++books;
        last_spread_bps = bt.spread().to_double() / bt.mid().to_double() * 1e4;
    }
    void on_trade(const llt::Trade&) { ++trades; }
    void on_depth(const llt::DepthSnapshot&) { ++depths; }
};

bool write_fixture() {
    llt::clk::clock_init();
    llt::md::RecordWriter w;
    if (!w.open(kPath)) return false;
    for (size_t i = 0; i < 3; ++i) {
        if (!w.write(1000 + i, 2000 + i, kPayloads[i])) return false;
    }
    w.close();
    return true;
}

}  // namespace

LLT_TEST(record_roundtrip_preserves_payloads_and_timestamps) {
    LLT_CHECK(write_fixture());

    llt::md::RecordReader r;
    LLT_CHECK(r.open(kPath));
    LLT_CHECK_EQ(r.header().magic, llt::md::kMagic);
    LLT_CHECK_EQ(r.header().version, llt::md::kVersion);

    llt::md::RecordHeader hdr;
    std::string_view      payload;

    for (size_t i = 0; i < 3; ++i) {
        LLT_CHECK(r.next(hdr, payload));
        LLT_CHECK_EQ(hdr.recv_tsc, 1000ULL + i);
        LLT_CHECK_EQ(hdr.recv_wall_ns, 2000ULL + i);
        LLT_CHECK(payload == kPayloads[i]);
    }

    LLT_CHECK(!r.next(hdr, payload));
    LLT_CHECK(!r.truncated());
}

LLT_TEST(record_rejects_a_file_that_is_not_a_recording) {
    const char* bogus = "/tmp/llt_record_bogus.bin";
    std::FILE*  f     = std::fopen(bogus, "wb");
    LLT_CHECK(f != nullptr);
    if (f == nullptr) return;
    std::fputs("not a recording at all, just text", f);
    std::fclose(f);

    llt::md::RecordReader r;
    LLT_CHECK(!r.open(bogus));
    std::remove(bogus);
}

LLT_TEST(record_reports_a_truncated_tail) {
    LLT_CHECK(write_fixture());

    // Chop the last payload in half, as a recorder killed mid-write would leave it.
    std::FILE* f = std::fopen(kPath, "rb");
    LLT_CHECK(f != nullptr);
    if (f == nullptr) return;
    std::vector<char> all;
    char              buf[4096];
    size_t            n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.insert(all.end(), buf, buf + n);
    std::fclose(f);

    const char* cut = "/tmp/llt_record_cut.mdlog";
    f               = std::fopen(cut, "wb");
    LLT_CHECK(f != nullptr);
    if (f == nullptr) return;
    std::fwrite(all.data(), 1, all.size() - 30, f);
    std::fclose(f);

    llt::md::RecordReader r;
    LLT_CHECK(r.open(cut));

    llt::md::RecordHeader hdr;
    std::string_view      payload;
    size_t                read = 0;
    while (r.next(hdr, payload)) ++read;

    LLT_CHECK_EQ(read, 2U);
    LLT_CHECK(r.truncated());
    std::remove(cut);
}

LLT_TEST(replay_decodes_through_the_live_dispatch_path) {
    LLT_CHECK(write_fixture());

    llt::md::RecordReader r;
    LLT_CHECK(r.open(kPath));

    llt::binance::FeedStats stats;
    Counter                 counter;

    llt::md::RecordHeader hdr;
    std::string_view      payload;
    while (r.next(hdr, payload)) {
        static_cast<void>(
            llt::binance::dispatch_payload(payload, hdr.recv_tsc, stats, counter));
    }

    LLT_CHECK_EQ(counter.books, 1ULL);
    LLT_CHECK_EQ(counter.trades, 1ULL);
    LLT_CHECK_EQ(counter.depths, 1ULL);
    LLT_CHECK_EQ(stats.decode_errors, 0ULL);
    LLT_CHECK_EQ(stats.ignored, 0ULL);
}

LLT_TEST(replay_is_deterministic_across_runs) {
    LLT_CHECK(write_fixture());

    const auto run = [] {
        llt::md::RecordReader   r;
        llt::binance::FeedStats stats;
        Counter                 c;
        if (!r.open(kPath)) return c;

        llt::md::RecordHeader hdr;
        std::string_view      payload;
        while (r.next(hdr, payload)) {
            static_cast<void>(llt::binance::dispatch_payload(payload, hdr.recv_tsc, stats, c));
        }
        return c;
    };

    const Counter a = run();
    const Counter b = run();

    LLT_CHECK_EQ(a.books, b.books);
    LLT_CHECK_EQ(a.trades, b.trades);
    LLT_CHECK_EQ(a.depths, b.depths);
    LLT_CHECK(a.last_spread_bps == b.last_spread_bps);

    std::remove(kPath);
}
