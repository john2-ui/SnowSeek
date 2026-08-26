#include "storage/index_file.hpp"
#include "storage/index_header.hpp"

#include "storage_test_fixture.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

namespace {

using snowseek::test::storage_fixture::make_index;
using snowseek::test::storage_fixture::read_bytes;
using snowseek::test::storage_fixture::TemporaryDirectory;

/** @brief Verifies deterministic serialization and complete logical reload. */
void round_trips_complete_index() {
        const TemporaryDirectory temporary("index-file-roundtrip");
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index;
        make_index(documents, index);
        const auto first = temporary.path() / "first.idx";
        const auto second = temporary.path() / "second.idx";
        const auto stats =
                snowseek::storage::write_index_file(first, documents, index);
        static_cast<void>(
                snowseek::storage::write_index_file(second, documents, index));

        snowseek::test::require_equal(read_bytes(first), read_bytes(second),
                                      "serialization should be deterministic");
        const auto loaded = snowseek::storage::read_index_file(first);
        const auto validated = snowseek::storage::validate_index_file(first);
        snowseek::test::require_equal(loaded.documents.size(), std::size_t{2},
                                      "both documents should reload");
        snowseek::test::require_equal(loaded.documents.get(0).path,
                                      std::filesystem::path("a.txt"),
                                      "relative paths should round-trip");
        snowseek::test::require_equal(loaded.documents.get(0).modified_time_ns,
                                      std::int64_t{-10},
                                      "signed timestamps should round-trip");
        snowseek::test::require_equal(
                loaded.documents.get(1).path,
                std::filesystem::path(std::u8string(u8"目录/文档.txt")),
                "generic UTF-8 paths should round-trip");
        snowseek::test::require_equal(loaded.documents.get(0).content_crc32c,
                                      std::optional<std::uint32_t>{0x12345678U},
                                      "v2 content CRC32C should round-trip");
        const auto *retry = loaded.index.find("retry");
        snowseek::test::require(retry != nullptr && retry->size() == 2,
                                "posting lists should reload");
        snowseek::test::require_equal(
                (*retry)[0].positions,
                std::vector<snowseek::index::Position>{0, 2},
                "positions should round-trip");
        snowseek::test::require_equal(stats.physical_document_count,
                                      std::uint64_t{2},
                                      "document statistics should match");
        snowseek::test::require_equal(stats.term_count, std::uint64_t{2},
                                      "term statistics should match");
        snowseek::test::require_equal(stats.posting_count, std::uint64_t{3},
                                      "posting statistics should match");
        snowseek::test::require_equal(stats.position_count, std::uint64_t{4},
                                      "position statistics should match");
        snowseek::test::require_equal(
                validated.file_size, stats.file_size,
                "streaming validation should report the serialized size");
        snowseek::test::require_equal(
                validated.position_count, stats.position_count,
                "streaming validation should report logical positions");
}

/** @brief Verifies v2 Tombstone encoding and Posting exclusion. */
void round_trips_tombstones() {
        const TemporaryDirectory temporary("index-file-roundtrip");
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index;
        const auto live = documents.add("live.txt", 4, 7, 0x10203040U);
        documents.set_token_count(live, 1);
        const auto tombstone = documents.add_tombstone("removed.txt");
        index.add_occurrence("live", live, 0);
        const auto path = temporary.path() / "tombstones.idx";
        const auto stats =
                snowseek::storage::write_index_file(path, documents, index);
        const auto loaded = snowseek::storage::read_index_file(path);
        std::ifstream input(path, std::ios::binary);
        const auto header = snowseek::storage::read_header(input);
        snowseek::test::require(
                header.version == snowseek::storage::kIndexFormatVersion &&
                        header.sections[0].length == 8 + 2 * 48 &&
                        stats.live_document_count == 1 &&
                        stats.tombstone_count == 1 &&
                        loaded.documents.get(tombstone).state ==
                                snowseek::document::DocumentState::tombstone,
                "v2 should store 48-byte live and Tombstone records");

        snowseek::index::InMemoryIndex invalid;
        invalid.add_occurrence("invalid", tombstone, 0);
        snowseek::test::require_throws<std::runtime_error>(
                [&temporary, &documents, &invalid] {
                        static_cast<void>(snowseek::storage::write_index_file(
                                temporary.path() / "invalid-tombstone.idx",
                                documents, invalid));
                },
                "a Tombstone must never receive a Posting");
}

/** @brief Verifies the v2 representation without a Positions section. */
void round_trips_positionless_index() {
        const TemporaryDirectory temporary("index-file-roundtrip");
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index(false);
        make_index(documents, index);
        const auto path = temporary.path() / "positionless.idx";
        const auto stats =
                snowseek::storage::write_index_file(path, documents, index);

        std::ifstream input(path, std::ios::binary);
        const auto header = snowseek::storage::read_header(input);
        const auto loaded = snowseek::storage::read_index_file(path);
        const auto *retry = loaded.index.find("retry");
        snowseek::test::require(
                header.feature_flags == 0 &&
                        header.sections.back().length == 0 &&
                        stats.position_count == 0 &&
                        !loaded.index.stores_positions(),
                "positionless v2 should clear its flag and section");
        snowseek::test::require(
                retry != nullptr && (*retry)[0].term_frequency() == 2 &&
                        (*retry)[0].positions.empty(),
                "positionless reload should retain frequency only");
}

} // namespace

/** @brief Runs persistent-index file integration tests. */
int main() {
        return snowseek::test::run({
                {"round-trips a complete index", round_trips_complete_index},
                {"round-trips a positionless index",
                 round_trips_positionless_index},
                {"round-trips Tombstones", round_trips_tombstones},
        });
}
