#include "ranking/bm25.hpp"

#include "test_support.hpp"

#include <cmath>

namespace {

/** @brief Verifies absent terms and empty corpora contribute no score. */
void returns_zero_for_incomplete_statistics() {
        snowseek::test::require_equal(
                snowseek::ranking::bm25(0, 1, 10, 2, 10.0), 0.0,
                "absent term should score zero");
        snowseek::test::require_equal(
                snowseek::ranking::bm25(1, 1, 10, 0, 0.0), 0.0,
                "empty corpus should score zero");
}

/** @brief Verifies a stable known BM25 calculation. */
void matches_known_value() {
        const double score = snowseek::ranking::bm25(2, 1, 10, 3, 8.0);
        snowseek::test::require(
                std::abs(score - 1.260043420) < 0.000001,
                "BM25 calculation should match the reference value");
}

/** @brief Verifies additional occurrences increase relevance monotonically. */
void rewards_term_frequency() {
        const double once = snowseek::ranking::bm25(1, 2, 10, 10, 10.0);
        const double often = snowseek::ranking::bm25(4, 2, 10, 10, 10.0);
        snowseek::test::require(often > once,
                                "higher frequency should score higher");
}

} // namespace

/** @brief Runs the BM25 unit-test suite. */
int main() {
        return snowseek::test::run({
                {"returns zero for incomplete statistics",
                 returns_zero_for_incomplete_statistics},
                {"matches known value", matches_known_value},
                {"rewards term frequency", rewards_term_frequency},
        });
}
