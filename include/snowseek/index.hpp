/**
 * @file index.hpp
 * @brief Declares the public persistent-index maintenance API.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include(<span>)
#include <span>
#define SNOWSEEK_HAS_STD_SPAN 1
#endif
#endif

namespace snowseek {

#if defined(SNOWSEEK_HAS_STD_SPAN)
/** @brief Non-owning view of path patterns passed to remove. */
using PathPatternSpan = std::span<const std::string>;
#else
/**
 * @brief Non-owning path-pattern view for C++20 libraries predating span.
 *
 * GCC 9 and Clang 10 are retained in the build matrix even though their
 * standard libraries do not yet provide std::span.
 */
class PathPatternSpan {
      public:
        /** @brief Creates an empty view. */
        PathPatternSpan() = default;

        /** @brief Views an lvalue vector for the duration of a call. */
        PathPatternSpan(const std::vector<std::string> &patterns) noexcept
            : data_(patterns.data()), size_(patterns.size()) {}

        PathPatternSpan(std::vector<std::string> &&) = delete;

        /** @brief Returns the first viewed pattern. */
        [[nodiscard]] const std::string *begin() const noexcept {
                return data_;
        }

        /** @brief Returns one-past the final viewed pattern. */
        [[nodiscard]] const std::string *end() const noexcept {
                return size_ == 0 ? data_ : data_ + size_;
        }

        /** @brief Returns the number of viewed patterns. */
        [[nodiscard]] std::size_t size() const noexcept { return size_; }

        /** @brief Reports whether the view contains no pattern. */
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

      private:
        const std::string *data_{}; ///< Borrowed first pattern when nonempty.
        std::size_t size_{};        ///< Number of borrowed patterns.
};
#endif

/** @brief Selects one fixed resource policy for index maintenance. */
enum class ResourceProfile {
        minimal,
        balanced,
        performance,
};

/** @brief Configures an index writer with optional profile overrides. */
struct IndexOptions {
        ResourceProfile profile{ResourceProfile::balanced}; ///< Base policy.
        std::optional<std::uint64_t>
                memory_limit_bytes; ///< Byte cap; absent uses profile default.
        std::optional<std::uint64_t>
                temporary_space_limit_bytes; ///< Optional workspace byte cap.
        std::optional<std::size_t>
                worker_threads; ///< Parsers; absent uses profile default.
        std::optional<std::size_t>
                merge_fan_in; ///< Merge width; absent uses profile default.
};

/** @brief Describes whether an index operation changed the visible revision. */
enum class IndexOutcome {
        unchanged,
        published,
        compacted,
};

/** @brief Identifies the stage that produced a recoverable diagnostic. */
enum class DiagnosticStage {
        scan,
        document,
        cleanup,
        maintenance,
};

/** @brief Reports one recoverable index-maintenance problem. */
struct Diagnostic {
        DiagnosticStage stage{};   ///< Stage that recovered from the problem.
        std::filesystem::path path; ///< Related path, when one exists.
        std::string message;        ///< Human-readable problem description.
};

/** @brief Counts logical path changes made or inspected by one operation. */
struct ChangeCounts {
        std::uint64_t added{};    ///< Newly visible source paths.
        std::uint64_t modified{}; ///< Paths replaced by newer content.
        std::uint64_t removed{}; ///< Paths hidden by update or explicit removal.
        std::uint64_t unchanged{}; ///< Paths with unchanged fingerprints.
        std::uint64_t matched{};  ///< Live paths selected by remove.
        std::uint64_t discarded_records{}; ///< Records removed by compaction.
};

/** @brief Reports the externally useful resource and indexing measurements. */
struct BuildMetrics {
        std::uint64_t scanned_files{}; ///< Candidate files discovered.
        std::uint64_t indexed_files{}; ///< Documents committed successfully.
        std::uint64_t failed_files{}; ///< Documents skipped after failures.
        std::uint64_t indexed_bytes{}; ///< Source bytes committed.
        std::uint64_t token_count{};   ///< Normalized tokens committed.
        std::uint64_t peak_memory_bytes{}; ///< Peak logical-memory charge.
        std::uint64_t peak_temporary_bytes{}; ///< Peak workspace bytes.
        std::uint64_t temporary_segments{}; ///< Pre-publication Segments.
        std::uint64_t merge_passes{}; ///< Merge levels used.
        std::size_t worker_threads{}; ///< Selected parser concurrency.
        bool positions_enabled{}; ///< Whether Segments retain positions.
};

/** @brief Summarizes one rebuild or maintenance operation. */
struct IndexResult {
        IndexOutcome outcome{IndexOutcome::unchanged}; ///< Revision effect.
        std::uint64_t revision{}; ///< Visible Manifest generation afterward.
        std::uint64_t active_segments{}; ///< Segments selected by the revision.
        ChangeCounts changes; ///< Logical path and compaction counts.
        BuildMetrics metrics; ///< Work and resource measurements.
        std::vector<Diagnostic> diagnostics; ///< Ordered recoverable problems.
};

/** @brief Reports validated logical and physical index totals. */
struct IndexStats {
        std::uint64_t bytes{}; ///< Total active Segment bytes.
        std::uint64_t documents{}; ///< Visible live documents.
        std::uint64_t segments{}; ///< Active physical Segments.
        std::uint64_t tombstones{}; ///< Retained Tombstone records.
        std::uint64_t terms{}; ///< Visible logical terms.
        std::uint64_t postings{}; ///< Visible logical Postings.
        std::uint64_t positions{}; ///< Positions in retained Postings.
};

/**
 * @brief Maintains one persistent index directory.
 *
 * The writer binds resource settings and a destination directory. Each method
 * holds the directory writer lock through publication and leaves the previous
 * visible revision selected when a pre-commit failure occurs.
 */
class IndexWriter {
      public:
        /**
         * @brief Creates a writer for one destination directory.
         * @param index_directory Directory created by rebuild and required by
         * maintenance operations.
         * @param options Resource profile and explicit limit overrides.
         * @throws std::invalid_argument If an explicit limit is invalid.
         */
        explicit IndexWriter(std::filesystem::path index_directory,
                             IndexOptions options = {});

        /**
         * @brief Rebuilds the complete index from a source tree.
         * @param source Existing corpus root to scan.
         * @return Publication, change, resource, and diagnostic information.
         */
        [[nodiscard]] IndexResult rebuild(const std::filesystem::path &source);

        /**
         * @brief Synchronizes the index with a source tree.
         * @param source Existing corpus root to scan.
         * @return An unchanged result or the published delta information.
         */
        [[nodiscard]] IndexResult update(const std::filesystem::path &source);

        /**
         * @brief Publishes deletions for live paths matching POSIX Globs.
         * @param path_globs Nonempty case-sensitive patterns retained only for
         * the duration of the call.
         * @return An unchanged result or the published Tombstone information.
         */
        [[nodiscard]] IndexResult remove(PathPatternSpan path_globs);

        /**
         * @brief Rewrites visible records into one canonical Segment.
         * @return An unchanged result or the compacted publication information.
         */
        [[nodiscard]] IndexResult compact();

      private:
        std::filesystem::path index_directory_; ///< Managed index directory.
        IndexOptions options_; ///< Resource policy for maintenance calls.
};

/**
 * @brief Fully validates an index and returns its visible statistics.
 * @param index_directory Directory containing a Segment v2 Manifest revision.
 * @return Validated logical and physical totals.
 * @throws std::runtime_error If the Manifest or any active Segment is invalid.
 */
[[nodiscard]] IndexStats
validate_index(const std::filesystem::path &index_directory);

} // namespace snowseek

#if defined(SNOWSEEK_HAS_STD_SPAN)
#undef SNOWSEEK_HAS_STD_SPAN
#endif
