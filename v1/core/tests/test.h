// A minimal test harness.
//
// Catch2 and GoogleTest are both fine choices and the design document names
// them. This is here instead so the core builds and tests with nothing but a
// C++17 compiler and CMake -- which is the same property that makes the core
// worth having: no dependency, nothing to install, runs anywhere.

#ifndef AIB_TEST_H
#define AIB_TEST_H

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace aibtest {

inline int g_failures = 0;
inline int g_tests = 0;
inline const char* g_current = "";
inline bool g_current_failed = false;

inline void Fail(const char* file, int line, const std::string& message) {
    if (!g_current_failed) {
        std::printf("  FAIL  %s\n", g_current);
        g_current_failed = true;
    }
    std::printf("        %s:%d: %s\n", file, line, message.c_str());
}

}  // namespace aibtest

#define TEST(name) static void test_##name()

#define RUN(name)                                   \
    do {                                            \
        aibtest::g_current = #name;                 \
        aibtest::g_current_failed = false;          \
        ++aibtest::g_tests;                         \
        test_##name();                              \
        if (aibtest::g_current_failed) {            \
            ++aibtest::g_failures;                  \
        } else {                                    \
            std::printf("  ok    %s\n", #name);     \
        }                                           \
    } while (0)

#define CHECK(expr)                                                     \
    do {                                                               \
        if (!(expr)) {                                                 \
            aibtest::Fail(__FILE__, __LINE__, "expected: " #expr);     \
        }                                                              \
    } while (0)

#define CHECK_MSG(expr, msg)                                                  \
    do {                                                                      \
        if (!(expr)) {                                                        \
            aibtest::Fail(__FILE__, __LINE__,                                 \
                          std::string(msg) + "  (" #expr ")");                \
        }                                                                     \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        const auto a_ = (actual);                                            \
        const auto e_ = (expected);                                          \
        if (!(a_ == e_)) {                                                   \
            char buf_[256];                                                  \
            std::snprintf(buf_, sizeof(buf_),                                \
                          #actual ": got %lld, want %lld",                   \
                          static_cast<long long>(a_),                        \
                          static_cast<long long>(e_));                       \
            aibtest::Fail(__FILE__, __LINE__, buf_);                         \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                    \
    do {                                                                     \
        const double a_ = static_cast<double>(actual);                       \
        const double e_ = static_cast<double>(expected);                     \
        const double t_ = static_cast<double>(tol);                          \
        if (!(std::fabs(a_ - e_) <= t_)) {                                   \
            char buf_[256];                                                  \
            std::snprintf(buf_, sizeof(buf_),                                \
                          #actual ": got %.6f, want %.6f +/- %.6f "          \
                          "(off by %.6f)",                                   \
                          a_, e_, t_, std::fabs(a_ - e_));                   \
            aibtest::Fail(__FILE__, __LINE__, buf_);                         \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        const std::string a_ = (actual);                                     \
        const std::string e_ = (expected);                                   \
        if (a_ != e_) {                                                      \
            aibtest::Fail(__FILE__, __LINE__,                                \
                          #actual ": got \"" + a_ + "\", want \"" + e_ +     \
                              "\"");                                         \
        }                                                                    \
    } while (0)

#define TEST_MAIN(suite_name, body)                                          \
    int main() {                                                             \
        std::printf("%s\n", suite_name);                                     \
        body;                                                                \
        std::printf("%s: %d tests, %d failed\n", suite_name, aibtest::g_tests, \
                    aibtest::g_failures);                                    \
        return aibtest::g_failures == 0 ? 0 : 1;                             \
    }

#endif  // AIB_TEST_H
