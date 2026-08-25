#include "snowseek/query/posting_operations.hpp"

#include "test_support.hpp"

#include <initializer_list>
#include <vector>

namespace {

/**
 * @brief Builds a valid posting list from ordered document identifiers.
 * @param document_ids Strictly increasing identifiers for the fixture.
 * @return Posting list with one placeholder position per document.
 */
snowseek::index::PostingList make_postings(
        std::initializer_list<snowseek::document::DocumentId> document_ids) {
        snowseek::index::PostingList postings;
        postings.reserve(document_ids.size());
        for (const auto document_id : document_ids) {
                postings.push_back(
                        snowseek::index::Posting{document_id, 1, {0}});
        }
        return postings;
}

/** @brief Verifies that an empty input produces an empty intersection. */
void intersects_empty_lists() {
        const snowseek::index::PostingList empty;
        const auto populated = make_postings({1, 3, 5});
        snowseek::test::require(
                snowseek::query::intersect_document_ids(empty, populated)
                        .empty(),
                "an empty left list should produce no matches");
        snowseek::test::require(
                snowseek::query::intersect_document_ids(populated, empty)
                        .empty(),
                "an empty right list should produce no matches");
}

/** @brief Verifies that disjoint postings do not produce false matches. */
void intersects_disjoint_lists() {
        const auto left = make_postings({1, 3, 5});
        const auto right = make_postings({2, 4, 6});
        snowseek::test::require(
                snowseek::query::intersect_document_ids(left, right).empty(),
                "disjoint posting lists should not intersect");
}

/** @brief Verifies partial intersection and ascending result order. */
void intersects_partially_overlapping_lists() {
        const auto left = make_postings({1, 2, 4, 7, 9});
        const auto right = make_postings({0, 2, 3, 7, 10});
        const std::vector<snowseek::document::DocumentId> expected{2, 7};
        snowseek::test::require_equal(
                snowseek::query::intersect_document_ids(left, right), expected,
                "partial intersections should retain ascending common ids");
}

/** @brief Verifies that a posting list intersects with itself exactly. */
void intersects_identical_lists() {
        const auto postings = make_postings({0, 2, 8});
        const std::vector<snowseek::document::DocumentId> expected{0, 2, 8};
        snowseek::test::require_equal(
                snowseek::query::intersect_document_ids(postings, postings),
                expected, "identical lists should return every document id");
}

/** @brief Verifies linear advancement across sparse document identifiers. */
void intersects_sparse_lists() {
        const auto left = make_postings({1, 100, 10'000});
        const auto right = make_postings({0, 100, 200, 10'000});
        const std::vector<snowseek::document::DocumentId> expected{100, 10'000};
        snowseek::test::require_equal(
                snowseek::query::intersect_document_ids(left, right), expected,
                "sparse lists should retain distant common ids");
}

} // namespace

/** @brief Runs the posting-operation unit-test suite. */
int main() {
        return snowseek::test::run({
                {"intersects empty lists", intersects_empty_lists},
                {"intersects disjoint lists", intersects_disjoint_lists},
                {"intersects partially overlapping lists",
                 intersects_partially_overlapping_lists},
                {"intersects identical lists", intersects_identical_lists},
                {"intersects sparse lists", intersects_sparse_lists},
        });
}
