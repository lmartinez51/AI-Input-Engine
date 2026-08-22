#pragma once

// Minimal dependency-free test framework for AI Input Engine.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mt {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& Checks() {
    static int checks = 0;
    return checks;
}

inline int& Failures() {
    static int failures = 0;
    return failures;
}

inline const char*& CurrentTest() {
    static const char* current = "";
    return current;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        Registry().push_back({name, std::move(fn)});
    }
};

inline void ReportFailure(const char* file, int line, const std::string& msg) {
    ++Failures();
    std::printf("  FAIL %s:%d  %s\n", file, line, msg.c_str());
}

inline int RunAll() {
    int ran = 0;
    for (const auto& tc : Registry()) {
        CurrentTest() = tc.name;
        const int before = Failures();
        ++ran;
        std::printf("[ RUN ] %s\n", tc.name);
        std::fflush(stdout);
        try {
            tc.fn();
        } catch (const std::exception& e) {
            ++Failures();
            std::printf("  FAIL %s  unhandled exception: %s\n", tc.name, e.what());
        } catch (...) {
            ++Failures();
            std::printf("  FAIL %s  unhandled unknown exception\n", tc.name);
        }
        if (Failures() == before) {
            std::printf("[  OK ] %s\n", tc.name);
        } else {
            std::printf("[FAIL ] %s\n", tc.name);
        }
        std::fflush(stdout);
    }
    std::printf("\n%d tests, %d checks, %d failures\n", ran, Checks(), Failures());
    return Failures() == 0 ? 0 : 1;
}

} // namespace mt

#define TEST(name)                                                                    \
    static void name();                                                               \
    static ::mt::Registrar name##_reg(#name, name);                                   \
    static void name()

#define CHECK(expr)                                                                   \
    do {                                                                              \
        ++::mt::Checks();                                                             \
        if (!(expr)) {                                                                \
            ::mt::ReportFailure(__FILE__, __LINE__, "CHECK failed: " #expr);          \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        ++::mt::Checks();                                                             \
        const auto va = (a);                                                          \
        const auto vb = (b);                                                          \
        if (!(va == vb)) {                                                            \
            ::mt::ReportFailure(__FILE__, __LINE__,                                   \
                                std::string("CHECK_EQ failed: " #a " == " #b));       \
        }                                                                             \
    } while (0)

#define CHECK_NE(a, b)                                                                \
    do {                                                                              \
        ++::mt::Checks();                                                             \
        const auto va = (a);                                                          \
        const auto vb = (b);                                                          \
        if (!(va != vb)) {                                                            \
            ::mt::ReportFailure(__FILE__, __LINE__,                                   \
                                std::string("CHECK_NE failed: " #a " != " #b));       \
        }                                                                             \
    } while (0)

#define CHECK_GT(a, b)                                                                \
    do {                                                                              \
        ++::mt::Checks();                                                             \
        const auto va = (a);                                                          \
        const auto vb = (b);                                                          \
        if (!(va > vb)) {                                                             \
            ::mt::ReportFailure(__FILE__, __LINE__,                                   \
                                std::string("CHECK_GT failed: " #a " > " #b));        \
        }                                                                             \
    } while (0)

#define CHECK_LE(a, b)                                                                \
    do {                                                                              \
        ++::mt::Checks();                                                             \
        const auto va = (a);                                                          \
        const auto vb = (b);                                                          \
        if (!(va <= vb)) {                                                            \
            ::mt::ReportFailure(__FILE__, __LINE__,                                   \
                                std::string("CHECK_LE failed: " #a " <= " #b));       \
        }                                                                             \
    } while (0)

#define CHECK_STREQ(a, b)                                                             \
    do {                                                                              \
        ++::mt::Checks();                                                             \
        const std::wstring sa((a));                                                   \
        const std::wstring sb((b));                                                   \
        if (sa != sb) {                                                               \
            const auto lossy = [](const std::wstring& s) {                            \
                std::string out;                                                      \
                out.reserve(s.size());                                                \
                for (const wchar_t c : s) {                                           \
                    out.push_back(static_cast<char>(c));                              \
                }                                                                     \
                return out;                                                           \
            };                                                                        \
            ::mt::ReportFailure(__FILE__, __LINE__,                                   \
                                "CHECK_STREQ failed: '" + lossy(sa) + "' != '" +      \
                                    lossy(sb) + "'");                                 \
        }                                                                             \
    } while (0)

#define CHECK_THROWS(expr)                                                            \
    do {                                                                              \
        ++::mt::Checks();                                                             \
        bool thrown = false;                                                          \
        try {                                                                         \
            (void)(expr);                                                             \
        } catch (...) {                                                               \
            thrown = true;                                                            \
        }                                                                             \
        if (!thrown) {                                                                \
            ::mt::ReportFailure(__FILE__, __LINE__, "CHECK_THROWS failed: " #expr);   \
        }                                                                             \
    } while (0)