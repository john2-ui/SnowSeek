/**
 * @file document_store_test.cpp
 * @brief Verifies document identifier allocation, metadata storage, and bounds
 * checks.
 */

#include "document/document_store.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>

namespace {

/** @brief Verifies contiguous document identifiers and store size. */
void assigns_contiguous_document_ids() {
        snowseek::document::DocumentStore store;
        const auto first = store.add("first.txt", 10, 100);
        const auto second = store.add("nested/second.cpp", 20, 200);

        snowseek::test::require_equal(first, snowseek::document::DocumentId{0},
                                      "the first document id should be zero");
        snowseek::test::require_equal(
                second, snowseek::document::DocumentId{1},
                "document ids should be assigned contiguously");
        snowseek::test::require_equal(store.size(), std::size_t{2},
                                      "the store should track its size");
}

/** @brief Verifies storage and mutation of document metadata. */
void stores_and_updates_document_metadata() {
        snowseek::document::DocumentStore store;
        const auto id = store.add("source/main.cpp", 42, 1234);
        store.set_token_count(id, 7);

        const auto &document = store.get(id);
        snowseek::test::require_equal(document.id, id,
                                      "stored metadata should retain its id");
        snowseek::test::require_equal(document.path,
                                      std::filesystem::path("source/main.cpp"),
                                      "stored metadata should retain its path");
        snowseek::test::require_equal(document.file_size, std::uint64_t{42},
                                      "file size should be retained");
        snowseek::test::require_equal(document.modified_time_ns,
                                      std::int64_t{1234},
                                      "modified time should be retained");
        snowseek::test::require_equal(document.token_count, std::uint32_t{7},
                                      "token count should be updateable");
}

/** @brief Verifies that unknown document identifiers are rejected. */
void rejects_unknown_document_ids() {
        snowseek::document::DocumentStore store;
        static_cast<void>(store.add("only.txt", 1, 2));
        const auto missing = snowseek::document::DocumentId{1};

        snowseek::test::require_throws<std::out_of_range>(
                [&store] { static_cast<void>(store.get(missing)); },
                "reading an unknown document should fail");
        snowseek::test::require_throws<std::out_of_range>(
                [&store] { store.set_token_count(missing, 3); },
                "updating an unknown document should fail");
}

/** @brief Verifies capacity-based document memory estimates grow with data. */
void estimates_retained_memory() {
        snowseek::document::DocumentStore store;
        snowseek::test::require_equal(
                store.estimated_memory_bytes(), std::uint64_t{0},
                "an empty document store should retain no dynamic storage");

        static_cast<void>(store.add("first-document.txt", 1, 2));
        const auto first = store.estimated_memory_bytes();
        static_cast<void>(store.add("nested/second-document.cpp", 3, 4));
        const auto second = store.estimated_memory_bytes();
        snowseek::test::require(first > 0,
                                "one document should retain estimated memory");
        snowseek::test::require(
                second > first,
                "additional documents should increase the estimate");
}

} // namespace

/** @brief Runs the document-store unit-test suite. */
int main() {
        return snowseek::test::run({
                {"assigns contiguous document ids",
                 assigns_contiguous_document_ids},
                {"stores and updates document metadata",
                 stores_and_updates_document_metadata},
                {"rejects unknown document ids", rejects_unknown_document_ids},
                {"estimates retained memory", estimates_retained_memory},
        });
}
