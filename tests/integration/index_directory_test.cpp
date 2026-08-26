#include "snowseek/index/index_builder.hpp"
#include "snowseek/storage/index_file.hpp"
#include "snowseek/storage/index_manifest.hpp"
#include "storage/index_directory_internal.hpp"

#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class TemporaryDirectory {
      public:
        /** @brief Creates one unique root for directory-publication fixtures. */
        TemporaryDirectory() {
                path_ = std::filesystem::temp_directory_path() /
                        ("snowseek-index-directory-test-" +
                         std::to_string(std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count()));
                std::filesystem::create_directory(path_);
        }

        /** @brief Removes every fixture owned by this test scope. */
        ~TemporaryDirectory() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
        }

        [[nodiscard]] const std::filesystem::path &path() const noexcept {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

/** @brief Writes exact fixture bytes to a regular file. */
void write_file(const std::filesystem::path &path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
                throw std::runtime_error("failed to write directory fixture");
        }
}

/** @brief Creates a valid empty legacy M4 Segment without a Manifest. */
void write_legacy_segment(const std::filesystem::path &directory) {
        std::filesystem::create_directories(directory);
        static_cast<void>(snowseek::storage::write_index_file(
                directory / snowseek::storage::kSegmentFileName,
                snowseek::document::DocumentStore{},
                snowseek::index::InMemoryIndex{}));
}

/** @brief Verifies strict Manifest loading and legacy-only fallback. */
void loads_directories_without_masking_manifest_errors() {
        const TemporaryDirectory temporary;
        const auto legacy = temporary.path() / "legacy";
        write_legacy_segment(legacy);
        snowseek::test::require_equal(
                snowseek::storage::validate_index_directory(legacy)
                        .document_count,
                std::uint64_t{0},
                "a missing Manifest should load the fixed legacy Segment");

        write_file(legacy / snowseek::storage::kManifestFileName, "broken");
        snowseek::test::require_throws<std::runtime_error>(
                [&legacy] {
                        static_cast<void>(
                                snowseek::storage::read_index_directory(legacy));
                },
                "a corrupt Manifest must not fall back to the legacy Segment");

        const auto missing = temporary.path() / "missing";
        write_legacy_segment(missing);
        write_file(missing / snowseek::storage::kManifestFileName,
                   snowseek::storage::encode_manifest({1, 3, {2}}));
        snowseek::test::require_throws<std::runtime_error>(
                [&missing] {
                        static_cast<void>(
                                snowseek::storage::validate_index_directory(
                                        missing));
                },
                "a missing referenced Segment must not use the legacy file");

        const auto corrupt = temporary.path() / "corrupt";
        write_legacy_segment(corrupt);
        write_file(corrupt / snowseek::storage::segment_file_name(2),
                   "not a Segment");
        write_file(corrupt / snowseek::storage::kManifestFileName,
                   snowseek::storage::encode_manifest({1, 3, {2}}));
        snowseek::test::require_throws<std::runtime_error>(
                [&corrupt] {
                        static_cast<void>(
                                snowseek::storage::read_index_directory(corrupt));
                },
                "a corrupt active Segment must not use the legacy file");
}

/** @brief Verifies migration, generations, monotonic IDs, and owned cleanup. */
void publishes_generations_and_recovers_owned_orphans() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "document.txt", "oldterm");

        const auto first = snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require_equal(first.segment_id,
                                      snowseek::storage::SegmentId{1},
                                      "a new directory should begin at SegmentId 1");
        snowseek::test::require_equal(first.manifest_generation,
                                      std::uint64_t{1},
                                      "a new directory should begin at generation 1");
        write_file(source / "document.txt", "newterm");
        const auto second = snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require_equal(second.segment_id,
                                      snowseek::storage::SegmentId{2},
                                      "a full rebuild should increment SegmentId");
        snowseek::test::require_equal(second.manifest_generation,
                                      std::uint64_t{2},
                                      "a full rebuild should increment generation");
        snowseek::test::require(
                !std::filesystem::exists(
                        index / snowseek::storage::segment_file_name(1)),
                "the old Segment should be removed after commit");
        const auto loaded = snowseek::storage::read_index_directory(index);
        snowseek::test::require(loaded.index.find("newterm") != nullptr &&
                                        loaded.index.find("oldterm") == nullptr,
                                "directory readers should see only the new corpus");

        const auto unknown = index / "keep-me.txt";
        const auto orphan =
                index / snowseek::storage::segment_file_name(99);
        write_file(unknown, "unknown");
        write_file(orphan, "orphan");
        std::filesystem::create_directory(index / ".snowseek-build-leftover");
        write_file(index / ".snowseek-manifest-leftover", "temporary");
        const auto third = snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require_equal(
                third.segment_id, snowseek::storage::SegmentId{100},
                "a valid orphan filename should prevent SegmentId reuse");
        snowseek::test::require(std::filesystem::exists(unknown),
                                "recovery must preserve unknown files");
        snowseek::test::require(!std::filesystem::exists(orphan) &&
                                        !std::filesystem::exists(
                                                index /
                                                ".snowseek-build-leftover") &&
                                        !std::filesystem::exists(
                                                index /
                                                ".snowseek-manifest-leftover"),
                                "recovery should remove recognized leftovers");
}

/** @brief Verifies an M4 fixed Segment migrates as logical SegmentId 1. */
void migrates_legacy_index() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "document.txt", "replacement");
        write_legacy_segment(index);

        const auto result =
                snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require_equal(result.segment_id,
                                      snowseek::storage::SegmentId{2},
                                      "legacy migration should publish ID 2");
        snowseek::test::require_equal(result.manifest_generation,
                                      std::uint64_t{1},
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

/** @brief Terminates a child exactly after its selected publication boundary. */
void crash_at_selected_point(
        snowseek::storage::detail::PublishObservationPoint point) {
        if (point == crash_point) {
                ::_exit(86);
        }
}

/** @brief Removes directory write permission after the durable commit point. */
void deny_cleanup_after_commit(
        snowseek::storage::detail::PublishObservationPoint point) {
        if (point == snowseek::storage::detail::PublishObservationPoint::
                             manifest_directory_synced) {
                static_cast<void>(::chmod(cleanup_failure_directory.c_str(),
                                          0555));
        }
}

/** @brief Verifies post-commit cleanup failure is a warning, not rollback. */
void reports_post_commit_cleanup_failure() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "document.txt", "before");
        static_cast<void>(snowseek::index::IndexBuilder{}.build(source, index));
        write_file(source / "document.txt", "after");

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

        snowseek::test::require(!result.cleanup_errors.empty(),
                                "old Segment cleanup failure should be reported");
        const auto loaded = snowseek::storage::read_index_directory(index);
        snowseek::test::require(loaded.index.find("after") != nullptr,
                                "cleanup failure must not roll back new generation");
        const auto recovered =
                snowseek::index::IndexBuilder{}.build(source, index);
        snowseek::test::require(recovered.cleanup_errors.empty(),
                                "the next writer should retry cleanup");
}

/** @brief Verifies every forced interruption leaves one readable generation. */
void survives_each_publication_interruption() {
        using Point = snowseek::storage::detail::PublishObservationPoint;
        const std::array points{
                Point::candidate_synced, Point::segment_renamed,
                Point::segment_directory_synced, Point::manifest_synced,
                Point::manifest_renamed, Point::manifest_directory_synced};

        for (std::size_t point_index = 0; point_index < points.size();
             ++point_index) {
                const TemporaryDirectory temporary;
                const auto source = temporary.path() / "source";
                const auto index = temporary.path() / "index";
                std::filesystem::create_directory(source);
                write_file(source / "document.txt", "before");
                static_cast<void>(
                        snowseek::index::IndexBuilder{}.build(source, index));
                write_file(source / "document.txt", "after");

                crash_point = points[point_index];
                snowseek::storage::detail::set_publish_observer(
                        crash_at_selected_point);
                const auto child = ::fork();
                if (child == -1) {
                        throw std::runtime_error("failed to fork fault test");
                }
                if (child == 0) {
                        static_cast<void>(snowseek::index::IndexBuilder{}.build(
                                source, index));
                        ::_exit(0);
                }
                int status = 0;
                static_cast<void>(::waitpid(child, &status, 0));
                snowseek::storage::detail::set_publish_observer(nullptr);
                snowseek::test::require(WIFEXITED(status) &&
                                                WEXITSTATUS(status) == 86,
                                        "fault child should stop at its boundary");
                const auto readable =
                        snowseek::storage::read_index_directory(index);
                snowseek::test::require(
                        readable.index.find("before") != nullptr ||
                                readable.index.find("after") != nullptr,
                        "an interruption should expose a complete old or new generation");

                const auto recovered =
                        snowseek::index::IndexBuilder{}.build(source, index);
                snowseek::test::require(
                        recovered.cleanup_errors.empty() &&
                                snowseek::storage::validate_index_directory(index)
                                                .document_count == 1,
                        "the next writer should recover leftovers and publish");
        }
}

/** @brief Verifies flock excludes another writer and releases at scope exit. */
void excludes_concurrent_writers() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto index = temporary.path() / "index";
        std::filesystem::create_directory(source);
        std::filesystem::create_directory(index);
        write_file(source / "document.txt", "content");
        {
                snowseek::storage::detail::IndexDirectoryTransaction held(index);
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

/** @brief Runs directory loading, recovery, locking, and publication tests. */
int main() {
        return snowseek::test::run({
                {"loads directories without masking Manifest errors",
                 loads_directories_without_masking_manifest_errors},
                {"publishes generations and recovers owned orphans",
                 publishes_generations_and_recovers_owned_orphans},
                {"migrates legacy index", migrates_legacy_index},
                {"survives each publication interruption",
                 survives_each_publication_interruption},
                {"reports post-commit cleanup failure",
                 reports_post_commit_cleanup_failure},
                {"excludes concurrent writers", excludes_concurrent_writers},
        });
}
