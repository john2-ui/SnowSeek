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

inline void require(bool condition, std::string_view message) {
        if (!condition) {
                throw std::runtime_error(std::string(message));
        }
}

template <typename Actual, typename Expected>
void require_equal(const Actual &actual, const Expected &expected,
                   std::string_view message) {
        if (!(actual == expected)) {
                throw std::runtime_error(std::string(message));
        }
}

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
