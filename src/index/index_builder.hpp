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
        filesystem::ScanOptions scan_options;
        document::TextReadOptions read_options;
        analysis::TokenizerOptions tokenizer_options;
        bool store_positions{true};
};

struct BuildError {
        std::filesystem::path path;
        std::string message;
};

struct BuildMemoryStats {
        std::uint64_t metadata_bytes{};
        std::uint64_t reader_peak_bytes{};
        std::uint64_t token_peak_bytes{};
        std::uint64_t dictionary_bytes{};
        std::uint64_t posting_bytes{};
        std::uint64_t estimated_peak_bytes{};
};

struct InMemoryBuildStats {
        std::uint64_t scanned_files{};
        std::uint64_t indexed_files{};
        std::uint64_t failed_files{};
        std::uint64_t indexed_bytes{};
        std::uint64_t token_count{};
        BuildMemoryStats memory;
};

struct InMemoryBuildResult {
        document::DocumentStore documents;
        InMemoryIndex index;
        std::vector<filesystem::ScanError> scan_errors;
        std::vector<BuildError> document_errors;
        InMemoryBuildStats stats;
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
        InMemoryBuildOptions options_;
};

struct PersistentBuildResult {
        std::filesystem::path index_file;
        storage::SegmentId segment_id{};
        std::uint64_t manifest_generation{};
        bool published{};
        bool compacted{};
        std::uint64_t active_segment_count{};
        std::uint64_t added_files{};
        std::uint64_t modified_files{};
        std::uint64_t removed_files{};
        std::uint64_t unchanged_files{};
        std::uint64_t matched_files{};
        std::uint64_t discarded_records{};
        InMemoryBuildStats stats;
        std::vector<filesystem::ScanError> scan_errors;
        std::vector<BuildError> document_errors;
        std::vector<BuildError> cleanup_errors;
        std::vector<BuildError> maintenance_errors;
        std::uint64_t temporary_segment_count{};
        std::uint64_t temporary_peak_bytes{};
        std::uint64_t merge_pass_count{};
        std::uint64_t memory_peak_bytes{};
        std::size_t worker_thread_count{};
        bool positions_enabled{true};
};

struct PersistentBuildOptions {
        InMemoryBuildOptions in_memory_options;
        std::uint64_t segment_flush_threshold_bytes{
                kDefaultSegmentFlushThresholdBytes};
        std::uint64_t temporary_space_budget_bytes{
                kDefaultTemporarySpaceBudgetBytes};
        std::size_t merge_fan_in{kDefaultMergeFanIn};
        std::uint64_t memory_budget_bytes{kDefaultMemoryBudgetBytes};
        std::size_t worker_thread_count{kDefaultWorkerThreadCount};
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
        PersistentBuildOptions options_;
};

} // namespace snowseek::index
