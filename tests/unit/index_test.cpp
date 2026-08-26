/**
 * @file index_test.cpp
 * @brief Verifies in-memory posting construction, lookup, and document
 * statistics.
 */

#include "index/index.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

/** @brief Verifies posting construction across documents and positions. */
void builds_postings_for_multiple_documents() {
        snowseek::index::InMemoryIndex index;
        index.add_occurrence("timeout", 0, 1);
        index.add_occurrence("timeout", 0, 4);
        index.add_occurrence("timeout", 2, 0);

        const auto *postings = index.find("timeout");
        snowseek::test::require(postings != nullptr,
                                "an indexed term should be found");
        snowseek::test::require_equal(
                postings->size(), std::size_t{2},
                "the term should have postings for two documents");
        snowseek::test::require_equal(
                (*postings)[0].document_id, snowseek::document::DocumentId{0},
                "the first posting should retain its document id");
        snowseek::test::require_equal(
                (*postings)[0].positions,
                std::vector<snowseek::index::Position>{1, 4},
                "positions should remain strictly ordered");
        snowseek::test::require_equal(
                (*postings)[0].term_frequency(), std::uint32_t{2},
                "term frequency should equal the position count");
        snowseek::test::require_equal(
                (*postings)[1].document_id, snowseek::document::DocumentId{2},
                "a later document should create a new posting");
        snowseek::test::require_equal(
                (*postings)[1].positions,
                std::vector<snowseek::index::Position>{0},
                "positions may restart for a new document");
}

/** @brief Verifies enforcement of posting and position ordering. */
void rejects_invalid_posting_order() {
        snowseek::index::InMemoryIndex index;
        index.add_occurrence("term", 1, 3);

        snowseek::test::require_throws<std::invalid_argument>(
                [&index] { index.add_occurrence("term", 1, 3); },
                "duplicate positions should be rejected");
        snowseek::test::require_throws<std::invalid_argument>(
                [&index] { index.add_occurrence("term", 1, 2); },
                "decreasing positions should be rejected");
        snowseek::test::require_throws<std::invalid_argument>(
                [&index] { index.add_occurrence("term", 0, 4); },
                "decreasing document ids should be rejected");

        const auto *postings = index.find("term");
        snowseek::test::require(postings != nullptr,
                                "the original posting should remain present");
        snowseek::test::require_equal(
                postings->size(), std::size_t{1},
                "rejected occurrences should not add postings");
        snowseek::test::require_equal(
                (*postings)[0].positions,
                std::vector<snowseek::index::Position>{3},
                "rejected occurrences should not change positions");
}

/** @brief Verifies that empty dictionary terms are rejected atomically. */
void rejects_empty_terms() {
        snowseek::index::InMemoryIndex index;
        snowseek::test::require_throws<std::invalid_argument>(
                [&index] { index.add_occurrence({}, 0, 0); },
                "an empty term should be rejected");
        snowseek::test::require_equal(
                index.term_count(), std::size_t{0},
                "an invalid term should not modify the dictionary");
}

/** @brief Verifies that missing lookups do not mutate the dictionary. */
void missing_lookups_do_not_modify_the_dictionary() {
        snowseek::index::InMemoryIndex index;
        index.add_occurrence("known", 0, 0);
        const auto count = index.term_count();

        snowseek::test::require(index.find("missing") == nullptr,
                                "a missing term should return nullptr");
        snowseek::test::require_equal(
                index.term_count(), count,
                "a missing lookup should not modify the dictionary");
}

/** @brief Verifies dictionary and posting estimates track retained capacity. */
void estimates_retained_memory() {
        snowseek::index::InMemoryIndex index;
        const auto empty = index.estimated_memory_usage();
        snowseek::test::require_equal(
                empty.dictionary_bytes, std::uint64_t{0},
                "an empty dictionary should retain no estimated bytes");
        snowseek::test::require_equal(
                empty.posting_bytes, std::uint64_t{0},
                "an empty index should retain no posting bytes");

        index.add_occurrence("alpha", 0, 0);
        const auto first = index.estimated_memory_usage();
        index.add_occurrence("alpha", 0, 1);
        const auto repeated = index.estimated_memory_usage();
        index.add_occurrence("alpha", 1, 0);
        const auto second_posting = index.estimated_memory_usage();
        index.add_occurrence("beta", 0, 2);
        const auto second_term = index.estimated_memory_usage();

        snowseek::test::require(
                first.dictionary_bytes > 0 && first.posting_bytes > 0,
                "one occurrence should allocate both categories");
        snowseek::test::require_equal(
                repeated.dictionary_bytes, first.dictionary_bytes,
                "another position should not grow the dictionary");
        snowseek::test::require(repeated.posting_bytes > first.posting_bytes,
                                "another position should grow posting storage");
        snowseek::test::require_equal(
                second_posting.dictionary_bytes, repeated.dictionary_bytes,
                "another posting should not grow the dictionary");
        snowseek::test::require(second_posting.posting_bytes >
                                        repeated.posting_bytes,
                                "another posting should grow posting storage");
        snowseek::test::require(second_term.dictionary_bytes >
                                        second_posting.dictionary_bytes,
                                "a unique term should grow dictionary storage");
}

} // namespace

/** @brief Runs the in-memory-index unit-test suite. */
int main() {
        return snowseek::test::run({
                {"builds postings for multiple documents",
                 builds_postings_for_multiple_documents},
                {"rejects invalid posting order",
                 rejects_invalid_posting_order},
                {"rejects empty terms", rejects_empty_terms},
                {"missing lookups do not modify the dictionary",
                 missing_lookups_do_not_modify_the_dictionary},
                {"estimates retained memory", estimates_retained_memory},
        });
}
