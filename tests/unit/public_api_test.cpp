#include "test_support.hpp"

namespace snowseek::test {

/** @brief Verifies that the public index header compiled independently. */
bool index_header_is_self_contained();

/** @brief Verifies that the public search header compiled independently. */
bool search_header_is_self_contained();

/** @brief Verifies that the public version header compiled independently. */
bool version_header_is_self_contained();

} // namespace snowseek::test

namespace {

/** @brief Checks all supported public headers as separate translation units. */
void public_headers_are_self_contained() {
        snowseek::test::require(
                snowseek::test::index_header_is_self_contained(),
                "index.hpp should expose the grouped 0.2 API");
        snowseek::test::require(
                snowseek::test::search_header_is_self_contained(),
                "search.hpp should expose optional snippets");
        snowseek::test::require(
                snowseek::test::version_header_is_self_contained(),
                "version.hpp should expose version 0.2.0");
}

} // namespace

/** @brief Runs public API boundary tests. */
int main() {
        return snowseek::test::run({{"public headers are self-contained",
                                     public_headers_are_self_contained}});
}
