/**
 * @file index_builder_internal.hpp
 * @brief Shares internal parsing, budgeting, and publication primitives.
 */

#pragma once

#include "index/index_builder.hpp"
#include "storage/index_directory_internal.hpp"
#include "storage/segment_merge.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace snowseek::index::builder_detail {

struct FileMetadata {
        std::uint64_t file_size{}; ///< Exact source length in bytes.
        std::int64_t modified_time_ns{}; ///< Unix Epoch timestamp in nanoseconds.
};

class MemoryLimitExceeded : public std::runtime_error {
      public:
        /** @brief Creates the stable logical-memory limit diagnostic. */
        MemoryLimitExceeded();
};

class BuildMemoryBudget {
      public:
        /**
         * @brief Creates a classified logical-memory budget.
         * @param limit_bytes Maximum concurrently charged bytes.
         */
        explicit BuildMemoryBudget(std::uint64_t limit_bytes);

        /**
         * @brief Replaces one reservation while preserving the total limit.
         * @param current_bytes Bytes currently charged by the caller.
         * @param requested_bytes Replacement charge requested by the caller.
         * @throws MemoryLimitExceeded If the replacement exceeds the limit.
         */
        void resize(std::uint64_t current_bytes, std::uint64_t requested_bytes);

        /**
         * @brief Releases a previously accepted reservation without throwing.
         * @param bytes Maximum bytes to remove from the current charge.
         */
        void release(std::uint64_t bytes) noexcept;

        /**
         * @brief Returns the greatest accepted concurrent reservation.
         * @return Peak charged bytes.
         */
        [[nodiscard]] std::uint64_t peak_bytes() const noexcept;

      private:
        std::uint64_t limit_bytes_{}; ///< Maximum concurrent logical charge.
        mutable std::mutex mutex_; ///< Protects charge counters across workers.
        std::uint64_t used_bytes_{}; ///< Current accepted charge in bytes.
        std::uint64_t peak_bytes_{}; ///< Greatest accepted charge in bytes.
};

class MemoryReservation {
      public:
        MemoryReservation() = default;

        /**
         * @brief Binds an initially empty charge to a shared budget.
         * @param budget Budget that owns the aggregate charge.
         */
        explicit MemoryReservation(BuildMemoryBudget &budget) noexcept;

        MemoryReservation(const MemoryReservation &) = delete;
        MemoryReservation &operator=(const MemoryReservation &) = delete;

        /**
         * @brief Transfers ownership of an accepted memory charge.
         * @param other Reservation invalidated by the transfer.
         */
        MemoryReservation(MemoryReservation &&other) noexcept;

        /**
         * @brief Releases the old charge and takes another reservation.
         * @param other Reservation invalidated by the transfer.
         * @return This reservation after transfer.
         */
        MemoryReservation &operator=(MemoryReservation &&other) noexcept;

        /** @brief Releases the accepted charge. */
        ~MemoryReservation();

        /**
         * @brief Replaces this reservation's logical-memory charge.
         * @param bytes New classified byte count.
         * @throws MemoryLimitExceeded If the shared limit would be exceeded.
         */
        void resize(std::uint64_t bytes);

        /** @brief Releases all charged bytes and detaches from the budget. */
        void reset() noexcept;

        /**
         * @brief Returns bytes currently charged by this reservation.
         * @return Current classified byte count.
         */
        [[nodiscard]] std::uint64_t bytes() const noexcept;

      private:
        BuildMemoryBudget *budget_{}; ///< Non-owning charged budget, if bound.
        std::uint64_t bytes_{}; ///< Charge currently owned by this reservation.
};

class BuildWorkspace {
      public:
        /**
         * @brief Creates a unique private build directory below a destination.
         * @param parent Existing directory that owns the workspace.
         * @param budget_bytes Maximum logical bytes retained in the workspace.
         * @throws std::system_error If Linux cannot create the workspace.
         */
        BuildWorkspace(const std::filesystem::path &parent,
                       std::uint64_t budget_bytes);

        BuildWorkspace(const BuildWorkspace &) = delete;
        BuildWorkspace &operator=(const BuildWorkspace &) = delete;

        /** @brief Removes all unpublished files owned by this build. */
        ~BuildWorkspace();

        /**
         * @brief Returns the private directory owned by this object.
         * @return Stable workspace path for this object's lifetime.
         */
        [[nodiscard]] const std::filesystem::path &path() const noexcept;

        /**
         * @brief Returns bytes still permitted by the configured budget.
         * @return Remaining logical workspace capacity.
         */
        [[nodiscard]] std::uint64_t remaining_bytes() const noexcept;

        /**
         * @brief Checks budget and filesystem capacity before a temporary
         * write.
         * @param bytes Conservative additional bytes required by the operation.
         * @throws std::runtime_error If budget or filesystem capacity is
         * insufficient.
         */
        void require_additional(std::uint64_t bytes) const;

        /**
         * @brief Checks the logical budget and destination filesystem before
         * publication staging.
         * @param bytes Candidate-copy and Manifest bytes that will coexist with
         * retained workspace files.
         * @param index_directory Existing destination directory to inspect.
         * @throws std::runtime_error If the shared budget or destination
         * filesystem capacity is insufficient.
         */
        void require_publication_staging(
                std::uint64_t bytes,
                const std::filesystem::path &index_directory) const;

        /**
         * @brief Accounts for one completed file retained by the workspace.
         * @param bytes Logical file length to add.
         * @throws std::runtime_error If the completed file exceeds the budget.
         */
        void add_file(std::uint64_t bytes);

        /**
         * @brief Records transient bytes coexisting with retained files.
         * @param bytes Additional transient byte count.
         * @throws std::runtime_error If the observed peak exceeds the budget.
         */
        void note_transient_peak(std::uint64_t bytes);

        /**
         * @brief Deletes and releases one retained temporary file.
         * @param path File owned by this workspace.
         * @param bytes Accounted logical length of the file.
         * @throws std::runtime_error If deletion or accounting fails.
         */
        void remove_file(const std::filesystem::path &path,
                         std::uint64_t bytes);

        /**
         * @brief Returns the observed logical workspace byte peak.
         * @return Greatest retained-plus-transient byte count.
         */
        [[nodiscard]] std::uint64_t peak_bytes() const noexcept;

      private:
        std::filesystem::path path_; ///< Private directory removed on destruction.
        std::uint64_t budget_bytes_{}; ///< Maximum retained and transient bytes.
        std::uint64_t used_bytes_{}; ///< Bytes retained by completed files.
        std::uint64_t peak_bytes_{}; ///< Peak retained-plus-transient bytes.
};

struct ParsedDocument {
        FileMetadata metadata; ///< Metadata stable across the source read.
        std::uint32_t content_crc32c{}; ///< CRC32C of raw source bytes.
        document::TextReadStats read_stats; ///< Reader measurements for the file.
        std::vector<analysis::Token> tokens; ///< Tokens in source order.
        std::uint64_t token_term_bytes{}; ///< Dynamic token text bytes.
        BuildMemoryStats memory_stats; ///< Transient parse-memory observations.
        MemoryReservation memory_reservation; ///< Charge held until commit.
};

/**
 * @brief Reads and validates metadata required for an indexed document.
 * @param path Candidate file whose metadata is requested.
 * @return Regular-file size and modification timestamp.
 * @throws std::system_error If metadata cannot be read.
 * @throws std::runtime_error If the path is not a valid regular file.
 * @throws std::overflow_error If its timestamp is unsupported.
 */
[[nodiscard]] FileMetadata read_metadata(const std::filesystem::path &path);

/**
 * @brief Reads and tokenizes one file without mutating an index.
 * @param path Source file to parse.
 * @param read_options Reader configuration used for streaming and estimates.
 * @param tokenizer_options Rules and limits applied during tokenization.
 * @param memory_budget Optional shared classified-memory budget.
 * @param observed_memory Optional aggregate receiving transient parse peaks.
 * @return Stable metadata, raw-content CRC32C, statistics, and tokens.
 * @throws std::runtime_error If metadata, reading, or UTF-8 validation fails.
 * @throws std::length_error If a token exceeds the configured limit.
 * @throws std::overflow_error If the token count exceeds std::uint32_t.
 */
[[nodiscard]] ParsedDocument
parse_document(const std::filesystem::path &path,
               const document::TextReadOptions &read_options,
               const analysis::TokenizerOptions &tokenizer_options,
               BuildMemoryBudget *memory_budget = nullptr,
               BuildMemoryStats *observed_memory = nullptr);

/**
 * @brief Commits a fully parsed document to a document table and index.
 * @param path Logical path stored in document metadata.
 * @param parsed Complete parsed state transferred into the destination.
 * @param documents Document table receiving metadata.
 * @param index Inverted index receiving occurrences.
 * @param stats Aggregate statistics updated after a complete commit.
 * @throws std::overflow_error If a counter or identifier exceeds its range.
 * @throws std::invalid_argument If posting order violates index invariants.
 */
void commit_document(const std::filesystem::path &path, ParsedDocument parsed,
                     document::DocumentStore &documents, InMemoryIndex &index,
                     InMemoryBuildStats &stats);

/**
 * @brief Merges transient parse peaks into persistent build statistics.
 * @param observed Per-document reader and token peaks.
 * @param aggregate Build-wide maxima mutated in place.
 */
void observe_parse_memory(const BuildMemoryStats &observed,
                          BuildMemoryStats &aggregate);

/**
 * @brief Estimates retained scanner candidate storage.
 * @param paths Candidate paths retained by a build.
 * @return Vector and path-character bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
estimated_path_list_bytes(const std::vector<std::filesystem::path> &paths);

/**
 * @brief Estimates retained scanner diagnostic storage.
 * @param errors Scanner errors retained by a build.
 * @return Vector and path-character bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
estimated_scan_error_bytes(const std::vector<filesystem::ScanError> &errors);

/**
 * @brief Estimates retained document diagnostic storage.
 * @param errors Document errors retained by a build.
 * @return Vector, path, and message bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
estimated_build_error_bytes(const std::vector<BuildError> &errors);

/**
 * @brief Estimates retained temporary Segment descriptor storage.
 * @param segments Segment descriptors retained for merging.
 * @return Vector capacity and path-character bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t estimated_segment_source_bytes(
        const std::vector<storage::detail::SegmentSource> &segments);

/**
 * @brief Recomputes the conservative classified-memory sum.
 * @param memory Memory categories whose total is updated.
 * @throws std::overflow_error If the total exceeds std::uint64_t.
 */
void update_estimated_peak(BuildMemoryStats &memory);

/**
 * @brief Finalizes memory categories for an in-memory build.
 * @param scan_paths Scanner candidates retained throughout the build.
 * @param result Completed result whose statistics are updated.
 * @throws std::overflow_error If an estimate exceeds std::uint64_t.
 */
void finalize_memory_stats(const std::vector<std::filesystem::path> &scan_paths,
                           InMemoryBuildResult &result);

/**
 * @brief Commits one workspace candidate and active Segment set.
 * @param publication Locked transaction owning the new identifier.
 * @param candidate Valid complete candidate file.
 * @param active_segments Manifest SegmentIds selected after commit.
 * @param workspace Workspace accounting candidate and Manifest staging.
 * @param stage_in_index_directory Whether to copy the candidate into a unique
 * index-directory staging file before publication.
 * @param memory_budget Classified memory budget for Manifest bytes.
 * @param result Operation result populated with publication diagnostics.
 * @throws std::runtime_error If validation, budget checks, or publication fail.
 */
void publish_candidate(storage::detail::IndexDirectoryTransaction &publication,
                       const std::filesystem::path &candidate,
                       std::vector<storage::SegmentId> active_segments,
                       BuildWorkspace &workspace,
                       bool stage_in_index_directory,
                       BuildMemoryBudget &memory_budget,
                       PersistentBuildResult &result);

} // namespace snowseek::index::builder_detail
