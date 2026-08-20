#include "snowseek/index/in_memory_index.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

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

void rejects_empty_terms() {
        snowseek::index::InMemoryIndex index;
        snowseek::test::require_throws<std::invalid_argument>(
                [&index] { index.add_occurrence({}, 0, 0); },
                "an empty term should be rejected");
        snowseek::test::require_equal(
                index.term_count(), std::size_t{0},
                "an invalid term should not modify the dictionary");
}

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

} // namespace

int main() {
        return snowseek::test::run({
                {"builds postings for multiple documents",
                 builds_postings_for_multiple_documents},
                {"rejects invalid posting order",
                 rejects_invalid_posting_order},
                {"rejects empty terms", rejects_empty_terms},
                {"missing lookups do not modify the dictionary",
                 missing_lookups_do_not_modify_the_dictionary},
        });
}
