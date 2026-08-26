/**
 * @file index_builder.hpp
 * @brief Declares in-memory and persistent index-building workflows.
 */

#pragma once

#include "analysis/tokenizer.hpp"
#include "document/document_store.hpp"
#include "document/text_reader.hpp"
#include "filesystem/scanner.hpp"
#include "index/index.hpp"
#include "storage/index_manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace snowseek::index {

inline constexpr std::uint64_t kDefaultSegmentFlushThresholdBytes =
        128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultTemporarySpaceBudgetBytes =
        std::numeric_limits<std::uint64_t>::max();
inline constexpr std::size_t kDefaultMergeFanIn = 16;
inline constexpr std::uint64_t kDefaultMemoryBudgetBytes =
        256ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kDefaultWorkerThreadCount = 2;

enum class ResourceProfile {
        minimal,
        balanced,
        performance,
};

struct InMemoryBuildOptions {
        filesystem::ScanOptions scan_options; ///< File discovery policy.
        document::TextReadOptions read_options; ///< Text decoding limits.
        analysis::TokenizerOptions tokenizer_options; ///< Tokenization policy.
        bool store_positions{true}; ///< Whether postings retain token positions.
};

struct BuildError {
        std::filesystem::path path; ///< Candidate associated with the failure.
        std::string message; ///< Human-readable diagnostic.
};

struct BuildMemoryStats {
        std::uint64_t metadata_bytes{}; ///< Retained metadata estimate in bytes.
        std::uint64_t reader_peak_bytes{}; ///< Peak reader allocation in bytes.
        std::uint64_t token_peak_bytes{}; ///< Peak token storage in bytes.
        std::uint64_t dictionary_bytes{}; ///< Peak term-map storage in bytes.
        std::uint64_t posting_bytes{}; ///< Peak posting storage in bytes.
        std::uint64_t estimated_peak_bytes{}; ///< Combined logical peak in bytes.
};

struct InMemoryBuildStats {
        std::uint64_t scanned_files{}; ///< Candidate files discovered.
        std::uint64_t indexed_files{}; ///< Documents committed successfully.
        std::uint64_t failed_files{}; ///< Documents rejected after discovery.
        std::uint64_t indexed_bytes{}; ///< Source bytes committed.
        std::uint64_t token_count{}; ///< Tokens committed across documents.
        BuildMemoryStats memory; ///< Logical memory observations.
};

struct InMemoryBuildResult {
        document::DocumentStore documents; ///< Successfully parsed documents.
        InMemoryIndex index; ///< Postings for successful documents.
        std::vector<filesystem::ScanError> scan_errors; ///< Discovery failures.
        std::vector<BuildError> document_errors; ///< Per-document failures.
        InMemoryBuildStats stats; ///< Aggregate build measurements.
};

class InMemoryIndexBuilder {
      public:
        /**
         * @brief Creates a builder and validates its reader and tokenizer
         * configuration.
         * @param options Options controlling file discovery, text reading, and
         * tokenization.
         * @throws std::invalid_argument If a reader or tokenizer option is
         * invalid.
         */
        explicit InMemoryIndexBuilder(InMemoryBuildOptions options = {});

        /**
         * @brief Builds a document table and optional positional index from a
         * source tree.
         * @param source Root directory scanned for candidate documents.
         * @return The successfully indexed documents, postings, diagnostics,
         * and aggregate statistics. Individual scan and document failures are
         * reported in the result and do not stop later candidates.
         * @throws std::overflow_error If an aggregate statistic or index
         * identifier exceeds its supported range.
         */
        [[nodiscard]] InMemoryBuildResult
        build(const std::filesystem::path &source) const;

      private:
        InMemoryBuildOptions options_; ///< Validated immutable build policy.
};

struct PersistentBuildResult {
        std::filesystem::path index_file; ///< Published Segment path, if any.
        storage::SegmentId segment_id{}; ///< Published Segment identifier.
        std::uint64_t manifest_generation{}; ///< Selected Manifest generation.
        bool published{}; ///< Whether a new generation was committed.
        bool compacted{}; ///< Whether active records were consolidated.
        std::uint64_t active_segment_count{}; ///< Segments in the selected view.
        std::uint64_t added_files{}; ///< Newly indexed live paths.
        std::uint64_t modified_files{}; ///< Reindexed existing live paths.
        std::uint64_t removed_files{}; ///< Tombstoned live paths.
        std::uint64_t unchanged_files{}; ///< Live paths reused unchanged.
        std::uint64_t matched_files{}; ///< Live paths selected for removal.
        std::uint64_t discarded_records{}; ///< Obsolete records dropped.
        InMemoryBuildStats stats; ///< Content and memory measurements.
        std::vector<filesystem::ScanError> scan_errors; ///< Discovery failures.
        std::vector<BuildError> document_errors; ///< Per-document failures.
        std::vector<BuildError> cleanup_errors; ///< Post-publication cleanup failures.
        std::vector<BuildError> maintenance_errors; ///< Recoverable maintenance failures.
        std::uint64_t temporary_segment_count{}; ///< Segments emitted before merge.
        std::uint64_t temporary_peak_bytes{}; ///< Peak workspace bytes.
        std::uint64_t merge_pass_count{}; ///< Merge levels completed.
        std::uint64_t memory_peak_bytes{}; ///< Peak charged memory bytes.
        std::size_t worker_thread_count{}; ///< Parsing workers used.
        bool positions_enabled{true}; ///< Whether published postings have token positions.
};

struct PersistentBuildOptions {
        InMemoryBuildOptions in_memory_options; ///< Scan, read, and token policy.
        std::uint64_t
                segment_flush_threshold_bytes{kDefaultSegmentFlushThresholdBytes}; ///< Flush cap.
        std::uint64_t
                temporary_space_budget_bytes{kDefaultTemporarySpaceBudgetBytes}; ///< Workspace cap.
        std::size_t merge_fan_in{kDefaultMergeFanIn}; ///< Inputs per merge group.
        std::uint64_t memory_budget_bytes{kDefaultMemoryBudgetBytes}; ///< Logical byte limit.
        std::size_t worker_thread_count{kDefaultWorkerThreadCount}; ///< Concurrent parsers.
};

/**
 * @brief Returns the fixed build settings for one resource profile.
 * @param profile Minimal, Balanced, or Performance resource policy.
 * @return Complete persistent build options for the selected profile.
 */
[[nodiscard]] PersistentBuildOptions
persistent_build_options(ResourceProfile profile);

class IndexBuilder {
      public:
        /**
         * @brief Creates a persistent builder with positive resource limits.
         * @param options Scanner, reader, tokenizer, and Segment batching
         * configuration.
         * @throws std::invalid_argument If a memory, temporary-space, flush, or
         * worker limit is zero, fan-in is below two, or nested configuration
         * is invalid.
         */
        explicit IndexBuilder(PersistentBuildOptions options = {});

        /**
         * @brief Prepares an index directory for a source tree.
         * @param source Existing source path to index.
         * @param index_directory Destination directory created when necessary.
         * @return Output path, build statistics, and recoverable diagnostics.
         * @throws std::runtime_error If source does not exist or persistence
         * fails.
         * @throws std::filesystem::filesystem_error If destination creation
         * fails.
         */
        [[nodiscard]] PersistentBuildResult
        build(const std::filesystem::path &source,
              const std::filesystem::path &index_directory) const;

        /**
         * @brief Synchronizes an existing index with one source tree.
         * @param source Existing source root scanned for live documents.
         * @param index_directory Existing Manifest or legacy index directory.
         * @return Publication statistics, or a no-op result when unchanged.
         * @throws std::runtime_error If scanning, fingerprinting, parsing, or
         * publication fails; the prior generation remains selected.
         */
        [[nodiscard]] PersistentBuildResult
        update(const std::filesystem::path &source,
               const std::filesystem::path &index_directory) const;

        /**
         * @brief Publishes Tombstones for live paths matching POSIX Globs.
         * @param index_directory Existing index directory to mutate.
         * @param glob_patterns Nonempty case-sensitive fnmatch patterns.
         * @return Publication statistics, or a no-op result when unmatched.
         * @throws std::invalid_argument If no pattern is supplied.
         * @throws std::runtime_error If validation or publication fails.
         */
        [[nodiscard]] PersistentBuildResult
        remove(const std::filesystem::path &index_directory,
               const std::vector<std::string> &glob_patterns) const;

        /**
         * @brief Rewrites all visible records into one canonical Segment.
         * @param index_directory Existing index directory to compact.
         * @return Publication statistics, or a no-op result when canonical.
         * @throws std::runtime_error If validation, budgets, or publication
         * fail; the prior generation remains selected.
         */
        [[nodiscard]] PersistentBuildResult
        compact(const std::filesystem::path &index_directory) const;

      private:
        PersistentBuildOptions options_; ///< Validated immutable build policy.
};

} // namespace snowseek::index
