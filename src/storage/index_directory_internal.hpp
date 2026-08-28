/**
 * @file index_directory_internal.hpp
 * @brief Declares internal index-directory loading and publication utilities.
 */

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
        std::filesystem::path path; ///< Path associated with the nonfatal failure.
        std::string message; ///< Human-readable nonfatal failure description.
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
        int fd_{-1}; ///< Owned POSIX descriptor, or -1 when empty.
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
 * Construction loads the current Manifest, removes only recognized SnowSeek
 * leftovers, and chooses identifiers that cannot reuse an orphaned SegmentId.
 * Callers validate or load the selected Segments while the lock is held. The
 * lock is released automatically when the object is destroyed.
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
         * @brief Fully validates Segments selected by the locked generation.
         *
         * A legacy fixed Segment is intentionally left unread so rebuild can
         * replace it through the normal Manifest commit protocol.
         *
         * @throws std::runtime_error If a selected v2 Segment is invalid.
         */
        void validate_current_segments() const;

        /**
         * @brief Loads the logical index selected by the locked generation.
         * @return Visible documents, postings, and physical statistics.
         * @throws std::runtime_error If no Manifest exists or a Segment is
         * invalid.
         */
        [[nodiscard]] LoadedIndex read_current_index() const;

        /**
         * @brief Copies a candidate into a unique staging file in the locked
         * index directory.
         * @param candidate Complete candidate stored on any filesystem.
         * @return Staging path suitable for same-filesystem publication.
         * @throws std::runtime_error If creation, copying, or closing fails.
         */
        [[nodiscard]] std::filesystem::path stage_candidate(
                const std::filesystem::path &candidate) const;

        /**
         * @brief Durably publishes a validated candidate and Manifest bytes.
         * @param candidate Owned staging file in the index filesystem.
         * @param manifest_bytes Prevalidated Manifest selecting segment_id().
         * @return Nonfatal errors encountered while deleting the old generation.
         * @throws std::runtime_error On any failure before the Manifest rename.
         */
        [[nodiscard]] std::vector<PublicationDiagnostic>
        publish(const std::filesystem::path &candidate,
                std::string_view manifest_bytes);

      private:
        std::filesystem::path directory_; ///< Locked index directory.
        std::vector<SegmentId> active_segments_; ///< Active IDs observed at lock acquisition.
        UniqueFd directory_fd_; ///< Directory descriptor carrying the writer lock.
        SegmentId segment_id_{}; ///< Identifier reserved for the candidate Segment.
        std::uint64_t generation_{}; ///< Manifest generation to publish.
        std::uint64_t current_generation_{}; ///< Generation observed at lock acquisition.
};

} // namespace snowseek::storage::detail
