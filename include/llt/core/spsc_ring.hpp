#pragma once

// Bounded lock-free single-producer / single-consumer queue.
//
// This is the only channel between threads in the engine. It never allocates, never
// blocks, and never takes a lock, so a slow consumer can stall but can never cause
// the producer to wait on it.
//
// The two indices live on separate cache lines, and each side keeps a private cached
// copy of the other's index. That cache is what makes this fast: in the common case
// neither side reads a cache line the other side is writing, so there is no coherence
// traffic per operation.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace llt {

inline constexpr size_t kCacheLine = 64;

template <typename T, size_t Capacity>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "hot-path queues carry plain data only");
    static_assert(Capacity >= 2, "capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

    static constexpr uint64_t kMask = Capacity - 1;

public:
    using value_type = T;

    SpscRing() = default;

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] bool try_push(const T& v) noexcept {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        if (w - read_cache_ >= Capacity) {
            read_cache_ = read_.load(std::memory_order_acquire);
            if (w - read_cache_ >= Capacity) return false;
        }
        buf_[w & kMask] = v;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        if (r == write_cache_) {
            write_cache_ = write_.load(std::memory_order_acquire);
            if (r == write_cache_) return false;
        }
        out = buf_[r & kMask];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size() const noexcept {
        const uint64_t w = write_.load(std::memory_order_acquire);
        const uint64_t r = read_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

private:
    alignas(kCacheLine) std::atomic<uint64_t> write_{0};
    uint64_t read_cache_{0};  // producer-private

    alignas(kCacheLine) std::atomic<uint64_t> read_{0};
    uint64_t write_cache_{0};  // consumer-private

    alignas(kCacheLine) std::array<T, Capacity> buf_{};
};

}  // namespace llt
