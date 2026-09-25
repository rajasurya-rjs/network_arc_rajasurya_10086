#pragma once

// A deliberately tiny test harness, so the project has no third-party
// dependencies. CHECK records a failure and continues. REQUIRE stops the
// current test case. The macros exist only to capture the expression text,
// file, and line, which a plain C++17 function cannot do.

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {

struct TestCase {
    const char* name;
    void (*run)();
};

struct RequireFailed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline int& failures() {
    static int count = 0;
    return count;
}

inline void report_failure(const std::string& what, const char* file, int line) {
    ++failures();
    std::cout << "    FAILED " << file << ':' << line << ": " << what << '\n';
}

template <typename A, typename B>
bool check_equal(const A& actual, const B& expected, const char* actual_text, const char* expected_text,
                 const char* file, int line) {
    if (actual == expected) return true;
    std::ostringstream message;
    message << actual_text << " == " << expected_text << "\n           actual:   " << actual
            << "\n           expected: " << expected;
    report_failure(message.str(), file, line);
    return false;
}

}  // namespace test

#define CHECK(condition) \
    ((condition) ? true : (::test::report_failure("CHECK(" #condition ")", __FILE__, __LINE__), false))

#define CHECK_EQ(actual, expected) ::test::check_equal((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define REQUIRE(condition)                                                         \
    do {                                                                           \
        if (!(condition)) {                                                        \
            ::test::report_failure("REQUIRE(" #condition ")", __FILE__, __LINE__); \
            throw ::test::RequireFailed(#condition);                               \
        }                                                                          \
    } while (false)
