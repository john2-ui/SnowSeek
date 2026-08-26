/**
 * @file index_maintenance_test.cpp
 * @brief Exercises incremental index updates, removals, and compaction.
 */

#include "index/index_builder.hpp"
#include "storage/index_file.hpp"
#include "storage/index_manifest.hpp"

#include "support/index_builder_fixture.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

using snowseek::test::read_file;
using snowseek::test::TemporaryDirectory;
using snowseek::test::write_file;

/** @brief Verifies incremental visibility, Tombstones, CRCs, and compaction. */
void updates_removes_and_compacts_multiple_segments() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "alpha");
        write_file(source / "b.txt", "beta");

        const snowseek::index::IndexBuilder builder;
        const auto initial = builder.build(source, destination);
        const auto unchanged = builder.update(source, destination);
        snowseek::test::require(
                !unchanged.published && unchanged.segment_id == 0 &&
                        unchanged.manifest_generation ==
                                initial.manifest_generation &&
                        unchanged.unchanged_files == 2,
                "an unchanged source should not consume a generation or ID");

        const auto original_time =
                std::filesystem::last_write_time(source / "a.txt");
        write_file(source / "a.txt", "omega");
        std::filesystem::last_write_time(source / "a.txt", original_time);
        write_file(source / "c.txt", "gamma");
        std::filesystem::remove(source / "b.txt");

        snowseek::index::PersistentBuildOptions denied_options;
        denied_options.memory_budget_bytes = 1;
        snowseek::test::require_throws<std::runtime_error>(
                [&source, &destination, &denied_options] {
                        static_cast<void>(
                                snowseek::index::IndexBuilder(denied_options)
                                        .update(source, destination));
                },
                "an incremental memory failure should abort publication");
        snowseek::test::require_equal(
                snowseek::storage::read_manifest_file(
                        destination / snowseek::storage::kManifestFileName)
                        .generation,
                initial.manifest_generation,
                "a failed update should preserve the selected generation");

        const auto updated = builder.update(source, destination);
        snowseek::test::require(
                updated.published && updated.added_files == 1 &&
                        updated.modified_files == 1 &&
                        updated.removed_files == 1 &&
                        updated.active_segment_count == 2,
                "one delta should encode added, modified, and removed paths");
        auto loaded = snowseek::storage::read_index_directory(destination);
        snowseek::test::require(
                loaded.documents.size() == 2 &&
                        loaded.index.find("omega") != nullptr &&
                        loaded.index.find("gamma") != nullptr &&
                        loaded.index.find("alpha") == nullptr &&
                        loaded.index.find("beta") == nullptr &&
                        loaded.stats.tombstone_count == 1,
                "newest records and Tombstones should determine visibility");
        for (const auto &document : loaded.documents.all()) {
                snowseek::test::require(document.content_crc32c.has_value(),
                                        "full and incremental live records "
                                        "should retain CRC32C");
        }

        const auto removed =
                builder.remove(destination, {"a.*", "a.*", "missing/**"});
        snowseek::test::require(
                removed.published && removed.matched_files == 1 &&
                        removed.active_segment_count == 3,
                "deduplicated Globs should publish one matching Tombstone");
        loaded = snowseek::storage::read_index_directory(destination);
        snowseek::test::require(
                loaded.documents.size() == 1 &&
                        loaded.documents.get(0).path ==
                                std::filesystem::path("c.txt"),
                "a remove delta should hide the selected live path");

        const auto restored = builder.update(source, destination);
        snowseek::test::require(
                restored.published && restored.added_files == 1 &&
                        restored.unchanged_files == 1,
                "update should re-add a removed path still present in source");
        const auto compacted = builder.compact(destination);
        snowseek::test::require(
                compacted.published && compacted.compacted &&
                        compacted.active_segment_count == 1 &&
                        compacted.discarded_records > 0,
                "compaction should replace hidden records with one Segment");
        loaded = snowseek::storage::read_index_directory(destination);
        snowseek::test::require(
                loaded.stats.segment_count == 1 &&
                        loaded.stats.tombstone_count == 0 &&
                        loaded.stats.physical_document_count == 2 &&
                        loaded.documents.size() == 2,
                "canonical compaction output should contain only live records");
        std::size_t published_files = 0;
        for (const auto &entry :
             std::filesystem::directory_iterator(destination)) {
                if (entry.is_regular_file()) {
                        ++published_files;
                }
        }
        snowseek::test::require_equal(published_files, std::size_t{2},
                                      "compaction should retain only MANIFEST "
                                      "and its active Segment");
        const auto canonical = builder.compact(destination);
        snowseek::test::require(
                !canonical.published && canonical.segment_id == 0,
                "compacting an already canonical Segment should be a no-op");
}

/** @brief Verifies the seventeenth active Segment triggers soft compaction. */
void automatically_compacts_above_the_soft_limit() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "document.txt", "value-00");
        const snowseek::index::IndexBuilder builder;
        static_cast<void>(builder.build(source, destination));

        snowseek::index::PersistentBuildResult result;
        for (std::uint32_t revision = 1; revision <= 16; ++revision) {
                write_file(source / "document.txt",
                           "value-" + std::to_string(revision));
                result = builder.update(source, destination);
        }
        snowseek::test::require(result.published && result.compacted &&
                                        result.active_segment_count == 1 &&
                                        result.maintenance_errors.empty(),
                                "a pending seventeenth Segment should compact "
                                "in its generation");
        const auto loaded =
                snowseek::storage::read_index_directory(destination);
        snowseek::test::require(
                loaded.stats.segment_count == 1 &&
                        loaded.documents.size() == 1 &&
                        loaded.index.find("15") == nullptr &&
                        loaded.index.find("16") != nullptr,
                "automatic compaction should retain only the newest version");
}

/** @brief Verifies missing CRC metadata forces a complete v2 replacement. */
void migrates_checksumless_live_records_even_to_empty() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        std::filesystem::create_directory(destination);
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index;
        const auto old = documents.add("old.txt", 3, 1);
        documents.set_token_count(old, 1);
        index.add_occurrence("old", old, 0);
        static_cast<void>(snowseek::storage::write_index_file(
                destination / snowseek::storage::segment_file_name(1),
                documents, index));
        write_file(destination / snowseek::storage::kManifestFileName,
                   snowseek::storage::encode_manifest({1, 2, {1}}));

        const auto migrated =
                snowseek::index::IndexBuilder{}.update(source, destination);
        const auto loaded =
                snowseek::storage::read_index_directory(destination);
        snowseek::test::require(migrated.published && migrated.compacted &&
                                        migrated.removed_files == 1 &&
                                        migrated.active_segment_count == 1 &&
                                        loaded.documents.size() == 0 &&
                                        loaded.stats.segment_count == 1,
                                "strict migration should publish an empty v2 "
                                "replacement when source is empty");
}

} // namespace

/** @brief Runs incremental-maintenance and compaction integration tests. */
int main() {
        return snowseek::test::run({
                {"updates, removes, and compacts multiple Segments",
                 updates_removes_and_compacts_multiple_segments},
                {"automatically compacts above the soft limit",
                 automatically_compacts_above_the_soft_limit},
                {"migrates checksumless live records",
                 migrates_checksumless_live_records_even_to_empty},
        });
}
