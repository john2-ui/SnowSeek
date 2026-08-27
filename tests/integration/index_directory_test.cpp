/**
 * @file index_directory_test.cpp
 * @brief Verifies index-directory publication, recovery, and generation
 * management.
 */

#include "index/index_builder.hpp"
#include "storage/index_file.hpp"
#include "storage/index_manifest.hpp"
#include "storage/index_directory_internal.hpp"

#include "storage_test_fixture.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using snowseek::test::storage_fixture::TemporaryDirectory;
using snowseek::test::storage_fixture::write_bytes;
using snowseek::test::storage_fixture::write_legacy_segment;

/** @brief Verifies migration, generations, monotonic IDs, and owned cleanup. */
void publishes_generations_and_recovers_owned_orphans() {
        const TemporaryDirectory temporary("index-directory-publication");
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_bytes(source / "document.txt", "oldterm");

        const auto first = snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require_equal(
                first.segment_id, snowseek::storage::SegmentId{1},
                "a new directory should begin at SegmentId 1");
        snowseek::test::require_equal(
                first.manifest_generation, std::uint64_t{1},
                "a new directory should begin at generation 1");
        write_bytes(source / "document.txt", "newterm");
        const auto second =
                snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require_equal(
                second.segment_id, snowseek::storage::SegmentId{2},
                "a full rebuild should increment SegmentId");
        snowseek::test::require_equal(
                second.manifest_generation, std::uint64_t{2},
                "a full rebuild should increment generation");
        snowseek::test::require(
                !std::filesystem::exists(
                        index / snowseek::storage::segment_file_name(1)),
                "the old Segment should be removed after commit");
        const auto loaded = snowseek::storage::read_index_directory(index);
        snowseek::test::require(
                loaded.index.find("newterm") != nullptr &&
                        loaded.index.find("oldterm") == nullptr,
                "directory readers should see only the new corpus");

        const auto unknown = index / "keep-me.txt";
        const auto orphan = index / snowseek::storage::segment_file_name(99);
        write_bytes(unknown, "unknown");
        write_bytes(orphan, "orphan");
        std::filesystem::create_directory(index / ".snowseek-build-leftover");
        write_bytes(index / ".snowseek-manifest-leftover", "temporary");
        write_bytes(index / ".snowseek-segment-leftover", "temporary");
        const auto third = snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require_equal(
                third.segment_id, snowseek::storage::SegmentId{100},
                "a valid orphan filename should prevent SegmentId reuse");
        snowseek::test::require(std::filesystem::exists(unknown),
                                "recovery must preserve unknown files");
        snowseek::test::require(
                !std::filesystem::exists(orphan) &&
                        !std::filesystem::exists(index /
                                                 ".snowseek-build-leftover") &&
                        !std::filesystem::exists(index /
                                                 ".snowseek-segment-leftover") &&
                        !std::filesystem::exists(index /
                                                 ".snowseek-manifest-leftover"),
                "recovery should remove recognized leftovers");
}

/** @brief Verifies a failed external copy removes its local staging file. */
void cleans_failed_candidate_copy() {
        const TemporaryDirectory temporary("index-directory-publication");
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_bytes(source / "document.txt", "content");
        const auto built =
                snowseek::index::IndexBuilder{}.build(source, index);

        {
                snowseek::storage::detail::IndexDirectoryTransaction transaction(
                        index);
                snowseek::test::require_throws<std::runtime_error>(
                        [&transaction, &temporary] {
                                static_cast<void>(transaction.stage_candidate(
                                        temporary.path() / "missing.idx"));
                        },
                        "copying a missing candidate should fail");
                for (const auto &entry :
                     std::filesystem::directory_iterator(index)) {
                        snowseek::test::require(
                                !entry.path().filename().string().starts_with(
                                        ".snowseek-segment-"),
                                "a failed copy should remove its staging file");
                }
        }

        snowseek::test::require_equal(
                snowseek::storage::read_manifest_file(
                        index / snowseek::storage::kManifestFileName)
                        .generation,
                built.manifest_generation,
                "a failed copy should preserve the visible generation");
}

/** @brief Verifies an M4 fixed Segment migrates as logical SegmentId 1. */
void migrates_legacy_index() {
        const TemporaryDirectory temporary("index-directory-publication");
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_bytes(source / "document.txt", "replacement");
        write_legacy_segment(index);

        const auto result =
                snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require_equal(result.segment_id,
                                      snowseek::storage::SegmentId{2},
                                      "legacy migration should publish ID 2");
        snowseek::test::require_equal(
                result.manifest_generation, std::uint64_t{1},
                "legacy migration should start generation 1");
        snowseek::test::require(
                std::filesystem::exists(index /
                                        snowseek::storage::kManifestFileName) &&
                        !std::filesystem::exists(
                                index / snowseek::storage::kSegmentFileName),
                "migration should commit MANIFEST before removing legacy ID 1");
}

snowseek::storage::detail::PublishObservationPoint crash_point;
std::filesystem::path cleanup_failure_directory;

/** @brief Terminates a child exactly after its selected publication boundary.
 */
void crash_at_selected_point(
        snowseek::storage::detail::PublishObservationPoint point) {
        if (point == crash_point) {
                ::_exit(86);
        }
}

/** @brief Verifies failed legacy rebuilds retain the fixed old Segment. */
void preserves_legacy_segment_until_manifest_commit() {
        using Point = snowseek::storage::detail::PublishObservationPoint;
        const TemporaryDirectory temporary("index-directory-publication");
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_bytes(source / "document.txt", "replacement");
        write_legacy_segment(index);

        crash_point = Point::manifest_synced;
        snowseek::storage::detail::set_publish_observer(
                crash_at_selected_point);
        const auto child = ::fork();
        if (child == -1) {
                snowseek::storage::detail::set_publish_observer(nullptr);
                throw std::runtime_error("failed to fork legacy fault test");
        }
        if (child == 0) {
                static_cast<void>(
                        snowseek::index::IndexBuilder{}.build(source, index));
                ::_exit(0);
        }
        int status = 0;
        static_cast<void>(::waitpid(child, &status, 0));
        snowseek::storage::detail::set_publish_observer(nullptr);

        snowseek::test::require(
                WIFEXITED(status) && WEXITSTATUS(status) == 86 &&
                        std::filesystem::exists(
                                index / snowseek::storage::kSegmentFileName) &&
                        !std::filesystem::exists(
                                index / snowseek::storage::kManifestFileName),
                "a pre-commit crash should leave the legacy layout intact");

        static_cast<void>(snowseek::index::IndexBuilder{}.build(source, index));
        snowseek::test::require(
                !std::filesystem::exists(index /
                                         snowseek::storage::kSegmentFileName) &&
                        std::filesystem::exists(
                                index / snowseek::storage::kManifestFileName),
                "the next rebuild should commit before retiring legacy data");
}

/** @brief Removes directory write permission after the durable commit point. */
void deny_cleanup_after_commit(
        snowseek::storage::detail::PublishObservationPoint point) {
        if (point == snowseek::storage::detail::PublishObservationPoint::
                             manifest_directory_synced) {
                static_cast<void>(
                        ::chmod(cleanup_failure_directory.c_str(), 0555));
        }
}

/** @brief Verifies post-commit cleanup failure is a warning, not rollback. */
void reports_post_commit_cleanup_failure() {
        const TemporaryDirectory temporary("index-directory-publication");
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_bytes(source / "document.txt", "before");
        static_cast<void>(snowseek::index::IndexBuilder{}.build(source, index));
        write_bytes(source / "document.txt", "after");

        cleanup_failure_directory = index;
        snowseek::storage::detail::set_publish_observer(
                deny_cleanup_after_commit);
        snowseek::index::PersistentBuildResult result;
        try {
                result = snowseek::index::IndexBuilder{}.build(source, index);
        } catch (...) {
                snowseek::storage::detail::set_publish_observer(nullptr);
                static_cast<void>(::chmod(index.c_str(), 0755));
                throw;
        }
        snowseek::storage::detail::set_publish_observer(nullptr);
        static_cast<void>(::chmod(index.c_str(), 0755));

        snowseek::test::require(
                !result.cleanup_errors.empty(),
                "old Segment cleanup failure should be reported");
        const auto loaded = snowseek::storage::read_index_directory(index);
        snowseek::test::require(
                loaded.index.find("after") != nullptr,
                "cleanup failure must not roll back new generation");
        const auto recovered =
                snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require(recovered.cleanup_errors.empty(),
                                "the next writer should retry cleanup");
}

/** @brief Verifies every forced interruption leaves one readable generation. */
void survives_each_publication_interruption() {
        using Point = snowseek::storage::detail::PublishObservationPoint;
        const std::array points{Point::candidate_synced,
                                Point::segment_renamed,
                                Point::segment_directory_synced,
                                Point::manifest_synced,
                                Point::manifest_renamed,
                                Point::manifest_directory_synced};

        for (std::size_t point_index = 0; point_index < points.size();
             ++point_index) {
                const TemporaryDirectory temporary(
                        "index-directory-publication");
                const auto source = temporary.path() / "source";
                const auto index = temporary.path() / "index";
                const auto workspace_parent = temporary.path() / "workspace";
                std::filesystem::create_directory(source);
                std::filesystem::create_directory(workspace_parent);
                write_bytes(source / "document.txt", "before");
                snowseek::index::PersistentBuildOptions options;
                options.temporary_directory = workspace_parent;
                const snowseek::index::IndexBuilder builder(options);
                static_cast<void>(builder.build(source, index));
                write_bytes(source / "document.txt", "after");

                crash_point = points[point_index];
                snowseek::storage::detail::set_publish_observer(
                        crash_at_selected_point);
                const auto child = ::fork();
                if (child == -1) {
                        throw std::runtime_error("failed to fork fault test");
                }
                if (child == 0) {
                        static_cast<void>(builder.build(source, index));
                        ::_exit(0);
                }
                int status = 0;
                static_cast<void>(::waitpid(child, &status, 0));
                snowseek::storage::detail::set_publish_observer(nullptr);
                snowseek::test::require(
                        WIFEXITED(status) && WEXITSTATUS(status) == 86,
                        "fault child should stop at its boundary");
                const auto readable =
                        snowseek::storage::read_index_directory(index);
                snowseek::test::require(
                        readable.index.find("before") != nullptr ||
                                readable.index.find("after") != nullptr,
                        "an interruption should expose a complete old or new "
                        "generation");

                const auto recovered = builder.build(source, index);
                snowseek::test::require(
                        recovered.cleanup_errors.empty() &&
                                snowseek::storage::validate_index_directory(
                                        index)
                                                .live_document_count == 1,
                        "the next writer should recover leftovers and publish");
        }
}

/** @brief Verifies delta publication crashes expose one complete generation. */
void survives_incremental_publication_interruptions() {
        using Point = snowseek::storage::detail::PublishObservationPoint;
        const std::array points{Point::candidate_synced,
                                Point::segment_renamed,
                                Point::segment_directory_synced,
                                Point::manifest_synced,
                                Point::manifest_renamed,
                                Point::manifest_directory_synced};

        for (const auto point : points) {
                const TemporaryDirectory temporary(
                        "index-directory-publication");
                const auto source = temporary.path() / "source";
                const auto index = temporary.path() / "index";
                std::filesystem::create_directory(source);
                write_bytes(source / "document.txt", "before");
                const snowseek::index::IndexBuilder builder;
                static_cast<void>(builder.build(source, index));
                write_bytes(source / "document.txt", "after");

                crash_point = point;
                snowseek::storage::detail::set_publish_observer(
                        crash_at_selected_point);
                const auto child = ::fork();
                if (child == -1) {
                        throw std::runtime_error(
                                "failed to fork incremental fault test");
                }
                if (child == 0) {
                        static_cast<void>(builder.update(source, index));
                        ::_exit(0);
                }
                int status = 0;
                static_cast<void>(::waitpid(child, &status, 0));
                snowseek::storage::detail::set_publish_observer(nullptr);
                snowseek::test::require(
                        WIFEXITED(status) && WEXITSTATUS(status) == 86,
                        "delta child should stop at its boundary");
                const auto readable =
                        snowseek::storage::read_index_directory(index);
                snowseek::test::require(
                        readable.index.find("before") != nullptr ||
                                readable.index.find("after") != nullptr,
                        "delta interruption should expose an old or new "
                        "generation");
                static_cast<void>(builder.update(source, index));
                const auto recovered =
                        snowseek::storage::read_index_directory(index);
                snowseek::test::require(
                        recovered.index.find("after") != nullptr &&
                                recovered.index.find("before") == nullptr,
                        "the next writer should recover and select the update");
        }
}

/** @brief Verifies flock excludes another writer and releases at scope exit. */
void excludes_concurrent_writers() {
        const TemporaryDirectory temporary("index-directory-publication");
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        std::filesystem::create_directory(index);
        write_bytes(source / "document.txt", "content");
        {
                snowseek::storage::detail::IndexDirectoryTransaction held(
                        index);
                snowseek::test::require_throws<std::runtime_error>(
                        [&source, &index] {
                                static_cast<void>(
                                        snowseek::index::IndexBuilder{}.build(
                                                source, index));
                        },
                        "a second writer must not cross the directory lock");
        }
        static_cast<void>(snowseek::index::IndexBuilder{}.build(source, index));
}

} // namespace

/** @brief Runs directory recovery, locking, and publication tests. */
int main() {
        return snowseek::test::run({
                {"publishes generations and recovers owned orphans",
                 publishes_generations_and_recovers_owned_orphans},
                {"cleans failed candidate copy", cleans_failed_candidate_copy},
                {"migrates legacy index", migrates_legacy_index},
                {"preserves legacy Segment until Manifest commit",
                 preserves_legacy_segment_until_manifest_commit},
                {"survives each publication interruption",
                 survives_each_publication_interruption},
                {"survives incremental publication interruptions",
                 survives_incremental_publication_interruptions},
                {"reports post-commit cleanup failure",
                 reports_post_commit_cleanup_failure},
                {"excludes concurrent writers", excludes_concurrent_writers},
        });
}
