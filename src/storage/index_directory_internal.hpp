#pragma once

#include "storage/index_file.hpp"
#include "storage/index_manifest.hpp"

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

/** @brief Owns one POSIX file descriptor and closes it at scope exit. */
class UniqueFd {
      public:
        /**
         * @brief Takes ownership of a descriptor.
         * @param fd Descriptor to own, or -1 for no descriptor.
         */
        explicit UniqueFd(int fd = -1) noexcept;

        /** @brief Closes the owned descriptor, ignoring close errors. */
        ~UniqueFd();

        /**
         * @brief Transfers descriptor ownership from another wrapper.
         * @param other Wrapper to empty.
         */
        UniqueFd(UniqueFd &&other) noexcept;

        /**
         * @brief Replaces the descriptor with one transferred from another.
         * @param other Wrapper to empty.
         * @return This wrapper.
         */
        UniqueFd &operator=(UniqueFd &&other) noexcept;

        UniqueFd(const UniqueFd &) = delete;
        UniqueFd &operator=(const UniqueFd &) = delete;

        /** @brief Returns the owned descriptor, or -1 when empty. */
        [[nodiscard]] int get() const noexcept;

        /**
         * @brief Closes the descriptor and reports a delayed close failure.
         * @param path Path named in an error diagnostic.
         * @throws std::runtime_error If close fails.
         */
        void close_checked(const std::filesystem::path &path);

      private:
        int fd_{-1};
};

/**
 * @brief Resolves newest path records across ordered loaded Segments.
 * @param segments Segment contents consumed in increasing persistent ID order.
 * @return One contiguous live document table and merged inverted index.
 * @throws std::runtime_error If Segment capabilities are inconsistent.
 */
[[nodiscard]] LoadedIndex
combine_loaded_indexes(std::vector<LoadedSegment> segments);

/**
 * @brief Applies one loaded delta Segment to a resolved logical baseline.
 * @param base Visible directory state consumed as the older logical baseline.
 * @param delta Newest Segment consumed after overriding matching paths in base.
 * @return Resolved logical contents and combined physical statistics.
 * @throws std::runtime_error If position capabilities are inconsistent.
 */
[[nodiscard]] LoadedIndex
combine_loaded_index_with_segment(LoadedIndex base, LoadedSegment delta);

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
        [[nodiscard]] std::uint64_t current_generation() const noexcept;
        [[nodiscard]] const std::vector<SegmentId> &
        active_segments() const noexcept;
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
        std::vector<SegmentId> active_segments_;
        UniqueFd directory_fd_;
        SegmentId segment_id_{};
        std::uint64_t generation_{};
        std::uint64_t current_generation_{};
};

} // namespace snowseek::storage::detail
