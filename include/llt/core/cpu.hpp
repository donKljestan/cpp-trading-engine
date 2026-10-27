#pragma once

// Thread placement helpers. Pinning matters more than it looks: an unpinned trading
// loop that migrates cores loses its L1/L2 working set and takes a multi-microsecond
// stall on the next market data event.

#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#    include <immintrin.h>
#endif

namespace llt::cpu {

[[nodiscard]] unsigned num_cores() noexcept;

// Pins the calling thread to a single core. Returns false if the core does not exist
// or the affinity call was refused.
[[nodiscard]] bool pin_to_core(unsigned core) noexcept;

[[nodiscard]] int current_core() noexcept;

// Name is truncated to 15 characters by the kernel. Shows up in top, perf and gdb.
bool set_thread_name(const char* name) noexcept;

// Hints to the core that this is a spin-wait iteration: yields pipeline resources to
// the sibling hyperthread and cuts the memory-order violation penalty on exit.
inline void spin_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

}  // namespace llt::cpu
