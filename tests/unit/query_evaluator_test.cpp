/**
 * @file query_evaluator_test.cpp
 * @brief Verifies prefix, proximity, and metadata query evaluation.
 */

#include "query/query_evaluator.hpp"
#include "query/query_parser.hpp"

#include "document/document_store.hpp"
#include "index/index.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct QueryFixture {
        snowseek::document::DocumentStore documents; ///< Controlled metadata.
        snowseek::index::InMemoryIndex index; ///< Controlled positional terms.

        /** @brief Creates three documents with ordered and gapped terms. */
        QueryFixture() {
                static_cast<void>(documents.add("before.txt", 0, -1));
                static_cast<void>(documents.add("epoch.txt", 1024, 0));
                static_cast<void>(documents.add("after.txt", 1ULL << 20U,
                                                86400000000000LL));

                index.add_occurrence("alpha", 0, 0);
                index.add_occurrence("alpha", 1, 0);
                index.add_occurrence("alpha", 2, 1);
                index.add_occurrence("alphabet", 0, 3);
                index.add_occurrence("alpine", 1, 3);
                index.add_occurrence("beta", 0, 2);
                index.add_occurrence("beta", 1, 1);
                index.add_occurrence("beta", 2, 0);
        }
};

/** @brief Evaluates an expression against one controlled fixture. */
[[nodiscard]] snowseek::query::QueryEvaluation
evaluate(const QueryFixture &fixture, const std::string &expression) {
        const auto query = snowseek::query::parse_query(expression);
        return snowseek::query::evaluate_query(*query, fixture.documents,
                                               fixture.index);
}

/** @brief Verifies prefix matching and concrete positive scoring terms. */
void expands_prefixes_once_for_matching_and_scoring() {
        const QueryFixture fixture;
        const auto result = evaluate(fixture, "alp*");
        snowseek::test::require_equal(
                result.documents, snowseek::query::DocumentIds({0, 1, 2}),
                "prefix query should unite all concrete posting lists");
        snowseek::test::require_equal(
                result.positive_terms,
                std::vector<std::string>({"alpha", "alphabet", "alpine"}),
                "prefix query should expose sorted concrete scoring terms");

        const auto negated = evaluate(fixture, "alpha NOT alp*");
        snowseek::test::require_equal(
                negated.positive_terms, std::vector<std::string>({"alpha"}),
                "effectively negated prefixes should not contribute scores");
}

/** @brief Verifies ordered phrase proximity boundaries and term order. */
void evaluates_ordered_phrase_proximity() {
        const QueryFixture fixture;
        snowseek::test::require_equal(
                evaluate(fixture, "\"alpha beta\"").documents,
                snowseek::query::DocumentIds({1}),
                "exact phrases should require adjacent positions");
        snowseek::test::require_equal(
                evaluate(fixture, "\"alpha beta\"~1").documents,
                snowseek::query::DocumentIds({0, 1}),
                "proximity should permit the configured extra span");
        snowseek::test::require_equal(
                evaluate(fixture, "\"beta alpha\"~5").documents,
                snowseek::query::DocumentIds({2}),
                "proximity should preserve source term order");
}

/** @brief Verifies all size relations against exact indexed byte counts. */
void evaluates_size_comparisons() {
        const QueryFixture fixture;
        snowseek::test::require_equal(
                evaluate(fixture, "size:0").documents,
                snowseek::query::DocumentIds({0}),
                "omitted size operator should mean equality");
        snowseek::test::require_equal(
                evaluate(fixture, "size:!=1KiB").documents,
                snowseek::query::DocumentIds({0, 2}),
                "size inequality should exclude exact values");
        snowseek::test::require_equal(
                evaluate(fixture, "size:<1KiB").documents,
                snowseek::query::DocumentIds({0}),
                "strict size lower comparison should work");
        snowseek::test::require_equal(
                evaluate(fixture, "size:<=1KiB").documents,
                snowseek::query::DocumentIds({0, 1}),
                "inclusive size lower comparison should work");
        snowseek::test::require_equal(
                evaluate(fixture, "size:>1KiB").documents,
                snowseek::query::DocumentIds({2}),
                "strict size greater comparison should work");
        snowseek::test::require_equal(
                evaluate(fixture, "size:>=1KiB").documents,
                snowseek::query::DocumentIds({1, 2}),
                "inclusive size greater comparison should work");
}

/** @brief Verifies UTC dates use floor division before the Unix Epoch. */
void evaluates_utc_mtime_days() {
        const QueryFixture fixture;
        snowseek::test::require_equal(
                evaluate(fixture, "mtime:1969-12-31").documents,
                snowseek::query::DocumentIds({0}),
                "negative sub-day timestamps should floor to the prior UTC "
                "day");
        snowseek::test::require_equal(
                evaluate(fixture, "mtime:>=1970-01-01").documents,
                snowseek::query::DocumentIds({1, 2}),
                "mtime ordering should compare UTC civil days");
        snowseek::test::require_equal(
                evaluate(fixture, "mtime:!=1970-01-01").documents,
                snowseek::query::DocumentIds({0, 2}),
                "mtime inequality should compare whole UTC days");
}

/** @brief Verifies the distinct prefix expansion ceiling is not truncated. */
void enforces_prefix_expansion_limit() {
        snowseek::document::DocumentStore documents;
        static_cast<void>(documents.add("terms.txt", 1, 0));
        snowseek::index::InMemoryIndex index;
        for (std::size_t term = 0;
             term < snowseek::query::kMaxExpandedPrefixTerms; ++term) {
                index.add_occurrence("prefix" + std::to_string(term), 0,
                                     static_cast<std::uint32_t>(term));
        }
        const auto query = snowseek::query::parse_query("prefix*");
        const auto boundary =
                snowseek::query::evaluate_query(*query, documents, index);
        snowseek::test::require_equal(
                boundary.positive_terms.size(),
                snowseek::query::kMaxExpandedPrefixTerms,
                "256 distinct expansions should remain valid");

        index.add_occurrence("prefix_overflow", 0, 256);
        snowseek::test::require_throws<std::invalid_argument>(
                [&documents, &index, &query] {
                        static_cast<void>(snowseek::query::evaluate_query(
                                *query, documents, index));
                },
                "257 distinct expansions should fail instead of truncating");
}

} // namespace

/** @brief Runs the query-evaluator unit-test suite. */
int main() {
        return snowseek::test::run({
                {"expands prefixes once for matching and scoring",
                 expands_prefixes_once_for_matching_and_scoring},
                {"evaluates ordered phrase proximity",
                 evaluates_ordered_phrase_proximity},
                {"evaluates size comparisons", evaluates_size_comparisons},
                {"evaluates UTC mtime days", evaluates_utc_mtime_days},
                {"enforces prefix expansion limit",
                 enforces_prefix_expansion_limit},
        });
}
