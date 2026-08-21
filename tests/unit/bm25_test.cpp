#include "snowseek/ranking/bm25.hpp"

#include "test_support.hpp"

namespace {

/** @brief Verifies that valid corpus statistics produce a positive score. */
void scores_valid_term() {
        const double score = snowseek::ranking::bm25(2, 3, 100, 10, 80.0);
        snowseek::test::require(score > 0.0,
                                "a present term should have a positive score");
}

/** @brief Verifies that incomplete statistics produce a zero score. */
void rejects_incomplete_statistics() {
        snowseek::test::require_equal(
                snowseek::ranking::bm25(0, 3, 100, 10, 80.0), 0.0,
                "zero term frequency should score zero");
        snowseek::test::require_equal(
                snowseek::ranking::bm25(2, 0, 100, 10, 80.0), 0.0,
                "zero document frequency should score zero");
        snowseek::test::require_equal(
                snowseek::ranking::bm25(2, 3, 100, 0, 80.0), 0.0,
                "zero document count should score zero");
        snowseek::test::require_equal(
                snowseek::ranking::bm25(2, 3, 100, 10, 0.0), 0.0,
                "zero average document length should score zero");
}

/** @brief Verifies that additional term occurrences increase the score. */
void rewards_higher_term_frequency() {
        const double low = snowseek::ranking::bm25(1, 3, 100, 10, 80.0);
        const double high = snowseek::ranking::bm25(4, 3, 100, 10, 80.0);
        snowseek::test::require(high > low,
                                "higher term frequency should increase score");
}

} // namespace

/** @brief Runs the BM25 unit-test suite. */
int main() {
        return snowseek::test::run({
                {"scores a valid term", scores_valid_term},
                {"rejects incomplete statistics",
                 rejects_incomplete_statistics},
                {"rewards higher term frequency",
                 rewards_higher_term_frequency},
        });
}
