#pragma once

#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace snowseek::test {

using TestFunction = void (*)();

struct TestCase {
        std::string_view name;
        TestFunction function;
};

/**
 * @brief Fails a test when a condition is false.
 * @param condition Assertion result.
 * @param message Diagnostic reported on failure.
 * @throws std::runtime_error If condition is false.
 */
inline void require(bool condition, std::string_view message) {
        if (!condition) {
                throw std::runtime_error(std::string(message));
        }
}

/**
 * @brief Fails a test when two values do not compare equal.
 * @tparam Actual Type of the observed value.
 * @tparam Expected Type of the expected value.
 * @param actual Observed value.
 * @param expected Expected value.
 * @param message Diagnostic reported on failure.
 * @throws std::runtime_error If the values differ.
 */
template <typename Actual, typename Expected>
void require_equal(const Actual &actual, const Expected &expected,
                   std::string_view message) {
        if (!(actual == expected)) {
                throw std::runtime_error(std::string(message));
        }
}

/**
 * @brief Verifies that a callable throws a requested exception type.
 * @tparam Exception Expected exception type.
 * @tparam Function Callable type.
 * @param function Operation expected to throw.
 * @param message Diagnostic reported on failure.
 * @throws std::runtime_error If no exception or a different type is thrown.
 */
template <typename Exception, typename Function>
void require_throws(Function &&function, std::string_view message) {
        try {
                std::forward<Function>(function)();
        } catch (const Exception &) {
                return;
        } catch (...) {
                throw std::runtime_error(std::string(message) +
                                         ": unexpected exception type");
        }
        throw std::runtime_error(std::string(message) +
                                 ": exception was not thrown");
}

/**
 * @brief Runs test cases and reports pass or failure diagnostics.
 * @param cases Named test functions to execute in order.
 * @return Zero when every case passes, otherwise one.
 */
inline int run(std::initializer_list<TestCase> cases) {
        int failures = 0;
        for (const auto &test_case : cases) {
                try {
                        test_case.function();
                        std::cout << "[PASS] " << test_case.name << '\n';
                } catch (const std::exception &error) {
                        ++failures;
                        std::cerr << "[FAIL] " << test_case.name << ": "
                                  << error.what() << '\n';
                } catch (...) {
                        ++failures;
                        std::cerr << "[FAIL] " << test_case.name
                                  << ": unknown exception\n";
                }
        }
        return failures == 0 ? 0 : 1;
}

} // namespace snowseek::test
