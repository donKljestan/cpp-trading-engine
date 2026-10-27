#include "harness.hpp"
#include "llt/core/spsc_ring.hpp"

#include <cstdint>
#include <thread>
#include <vector>

namespace {

struct Msg {
    uint64_t seq;
    uint64_t payload;
};

}  // namespace

LLT_TEST(spsc_push_pop_single_thread) {
    llt::SpscRing<int, 4> q;
    int                   v = 0;

    LLT_CHECK(q.empty());
    LLT_CHECK(!q.try_pop(v));

    LLT_CHECK(q.try_push(1));
    LLT_CHECK(q.try_push(2));
    LLT_CHECK_EQ(q.size(), 2U);

    LLT_CHECK(q.try_pop(v));
    LLT_CHECK_EQ(v, 1);
    LLT_CHECK(q.try_pop(v));
    LLT_CHECK_EQ(v, 2);
    LLT_CHECK(q.empty());
}

LLT_TEST(spsc_rejects_push_when_full) {
    llt::SpscRing<int, 4> q;
    for (int i = 0; i < 4; ++i) LLT_CHECK(q.try_push(i));

    LLT_CHECK_EQ(q.size(), 4U);
    LLT_CHECK(!q.try_push(99));

    int v = 0;
    LLT_CHECK(q.try_pop(v));
    LLT_CHECK(q.try_push(99));
}

LLT_TEST(spsc_wraps_around_many_times) {
    llt::SpscRing<int, 4> q;
    int                   v = 0;
    for (int i = 0; i < 10'000; ++i) {
        LLT_CHECK(q.try_push(i));
        LLT_CHECK(q.try_pop(v));
        LLT_CHECK_EQ(v, i);
    }
    LLT_CHECK(q.empty());
}

LLT_TEST(spsc_concurrent_preserves_order_and_payload) {
    constexpr uint64_t kCount = 200'000;
    llt::SpscRing<Msg, 1024>   q;

    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount; ++i) {
            const Msg m{i, i * 2654435761ULL};
            while (!q.try_push(m)) {
            }
        }
    });

    uint64_t expected = 0;
    uint64_t bad      = 0;
    while (expected < kCount) {
        Msg m{};
        if (!q.try_pop(m)) continue;
        if (m.seq != expected || m.payload != expected * 2654435761ULL) ++bad;
        ++expected;
    }
    producer.join();

    LLT_CHECK_EQ(bad, 0ULL);
    LLT_CHECK_EQ(expected, kCount);
    LLT_CHECK(q.empty());
}
