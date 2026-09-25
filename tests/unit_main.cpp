#include <csignal>
#include <iostream>
#include <vector>

#include "test_framework.hpp"

std::vector<test::TestCase> parser_tests();
std::vector<test::TestCase> router_tests();
std::vector<test::TestCase> connection_tests();

int main() {
    std::signal(SIGPIPE, SIG_IGN);  // same as the server: a reset peer must surface as EPIPE, not kill us

    std::vector<test::TestCase> all;
    for (auto suite : {parser_tests, router_tests, connection_tests}) {
        for (const test::TestCase& test_case : suite()) all.push_back(test_case);
    }

    int failed_cases = 0;
    for (const test::TestCase& test_case : all) {
        const int failures_before = test::failures();
        try {
            test_case.run();
        } catch (const test::RequireFailed&) {
            // already reported
        } catch (const std::exception& e) {
            test::report_failure(std::string("unexpected exception: ") + e.what(), __FILE__, __LINE__);
        }
        const bool passed = test::failures() == failures_before;
        if (!passed) ++failed_cases;
        std::cout << (passed ? "  PASS  " : "  FAIL  ") << test_case.name << '\n';
    }

    std::cout << '\n' << (all.size() - static_cast<std::size_t>(failed_cases)) << '/' << all.size()
              << " unit test cases passed\n";
    return failed_cases == 0 ? 0 : 1;
}
