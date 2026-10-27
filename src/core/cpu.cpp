#include "llt/core/cpu.hpp"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace llt::cpu {

unsigned num_cores() noexcept {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<unsigned>(n) : 1U;
}

bool pin_to_core(unsigned core) noexcept {
    if (core >= num_cores()) return false;

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);  // takes an unsigned directly; an int cast would widen with sign

    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

int current_core() noexcept { return ::sched_getcpu(); }

bool set_thread_name(const char* name) noexcept {
    return ::pthread_setname_np(::pthread_self(), name) == 0;
}

}  // namespace llt::cpu
