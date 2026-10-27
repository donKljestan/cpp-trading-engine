#pragma once

// Minimal test harness. Deliberately dependency-free: the build must work on a fresh
// machine with no network, and a test framework is not worth a FetchContent failure
// mode during CI bring-up.

#include <cstdio>
#include <string>
#include <vector>

namespace llt::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& current_failures() {
    static int f = 0;
    return f;
}

struct Register {
    Register(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

inline void report_failure(const char* file, int line, const std::string& what) {
    ++current_failures();
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
}

inline int run_all() {
    int failed_cases = 0;
    for (const Case& c : registry()) {
        current_failures() = 0;
        c.fn();
        if (current_failures() == 0) {
            std::printf("[ ok ] %s\n", c.name);
        } else {
            std::printf("[FAIL] %s (%d check(s))\n", c.name, current_failures());
            ++failed_cases;
        }
    }
    std::printf("\n%zu case(s), %d failed\n", registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace llt::test

#define LLT_TEST(name)                                           \
    static void name();                                          \
    static ::llt::test::Register llt_reg_##name{#name, &name};   \
    static void name()

#define LLT_CHECK(cond)                                                            \
    do {                                                                           \
        if (!(cond)) ::llt::test::report_failure(__FILE__, __LINE__, #cond);       \
    } while (0)

#define LLT_CHECK_EQ(a, b)                                                         \
    do {                                                                           \
        const auto llt_a_ = (a);                                                   \
        const auto llt_b_ = (b);                                                   \
        if (!(llt_a_ == llt_b_)) {                                                 \
            ::llt::test::report_failure(__FILE__, __LINE__,                        \
                                        std::string(#a " == " #b) + " [got " +     \
                                            std::to_string(llt_a_) + " vs " +      \
                                            std::to_string(llt_b_) + "]");         \
        }                                                                          \
    } while (0)
