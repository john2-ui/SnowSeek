#pragma once

#include "snowseek/storage/index_manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::storage::detail {

enum class PublishObservationPoint {
        candidate_synced,
        segment_renamed,
        segment_directory_synced,
        manifest_synced,
        manifest_renamed,
        manifest_directory_synced,
};

using PublishObserver = void (*)(PublishObservationPoint);

/** @brief Installs a process-local observer used only by publication tests. */
void set_publish_observer(PublishObserver observer) noexcept;

struct PublicationDiagnostic {
        std::filesystem::path path;
        std::string message;
};

/**
 * @brief Holds the directory writer lock and owns recovery/publication state.
 *
 * Construction validates the current generation, removes only recognized
 * SnowSeek leftovers, and chooses identifiers that cannot reuse an orphaned
 * SegmentId. The lock is released automatically when the object is destroyed.
 */
class IndexDirectoryTransaction {
      public:
        explicit IndexDirectoryTransaction(std::filesystem::path directory);
        ~IndexDirectoryTransaction();

        IndexDirectoryTransaction(const IndexDirectoryTransaction &) = delete;
        IndexDirectoryTransaction &
        operator=(const IndexDirectoryTransaction &) = delete;

        [[nodiscard]] SegmentId segment_id() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;
        [[nodiscard]] std::filesystem::path segment_path() const;

        /**
         * @brief Durably publishes a validated candidate and Manifest bytes.
         * @param candidate Candidate file in the same filesystem.
         * @param manifest_bytes Prevalidated Manifest selecting segment_id().
         * @return Nonfatal errors encountered while deleting the old generation.
         * @throws std::runtime_error On any failure before the Manifest rename.
         */
        [[nodiscard]] std::vector<PublicationDiagnostic>
        publish(const std::filesystem::path &candidate,
                std::string_view manifest_bytes);

      private:
        std::filesystem::path directory_;
        std::filesystem::path old_segment_;
        int directory_fd_{-1};
        SegmentId segment_id_{};
        std::uint64_t generation_{};
};

} // namespace snowseek::storage::detail
