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
        const std::string *data_{};
        std::size_t size_{};
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
        /** Base resource policy applied before explicit overrides. */
        ResourceProfile profile{ResourceProfile::balanced};
        /** Maximum classified build memory, or the profile default. */
        std::optional<std::uint64_t> memory_limit_bytes;
        /** Maximum retained and transient workspace bytes, or no override. */
        std::optional<std::uint64_t> temporary_space_limit_bytes;
        /** Concurrent document parsers, or the profile default. */
        std::optional<std::size_t> worker_threads;
        /** Maximum Segment inputs per merge, or the profile default. */
        std::optional<std::size_t> merge_fan_in;
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
        /** Operation stage that recovered from the problem. */
        DiagnosticStage stage{};
        /** Related source or index path, when one exists. */
        std::filesystem::path path;
        /** Human-readable problem description. */
        std::string message;
};

/** @brief Counts logical path changes made or inspected by one operation. */
struct ChangeCounts {
        /** Newly visible source paths. */
        std::uint64_t added{};
        /** Existing paths replaced by newer content. */
        std::uint64_t modified{};
        /** Paths hidden because they disappeared from the source tree. */
        std::uint64_t removed{};
        /** Scanned paths whose fingerprints did not change. */
        std::uint64_t unchanged{};
        /** Live paths selected by an explicit remove operation. */
        std::uint64_t matched{};
        /** Physical records removed by compaction. */
        std::uint64_t discarded_records{};
};

/** @brief Reports the externally useful resource and indexing measurements. */
struct BuildMetrics {
        /** Candidate files discovered by the scanner. */
        std::uint64_t scanned_files{};
        /** Documents committed successfully. */
        std::uint64_t indexed_files{};
        /** Documents skipped after recoverable failures. */
        std::uint64_t failed_files{};
        /** Source bytes read for committed documents. */
        std::uint64_t indexed_bytes{};
        /** Normalized tokens committed to the index. */
        std::uint64_t token_count{};
        /** Peak classified logical-memory charge. */
        std::uint64_t peak_memory_bytes{};
        /** Peak retained-plus-transient workspace bytes. */
        std::uint64_t peak_temporary_bytes{};
        /** Temporary Segments emitted before the published candidate. */
        std::uint64_t temporary_segments{};
        /** Merge levels used to produce the candidate. */
        std::uint64_t merge_passes{};
        /** Parser concurrency selected for the operation. */
        std::size_t worker_threads{};
        /** Whether the published Segment stores token positions. */
        bool positions_enabled{};
};

/** @brief Summarizes one rebuild or maintenance operation. */
struct IndexResult {
        /** Whether the visible revision changed and why. */
        IndexOutcome outcome{IndexOutcome::unchanged};
        /** Visible Manifest generation after the operation. */
        std::uint64_t revision{};
        /** Number of Segments selected by that generation. */
        std::uint64_t active_segments{};
        /** Logical source-path and compaction counts. */
        ChangeCounts changes;
        /** Concise work and resource measurements. */
        BuildMetrics metrics;
        /** Recoverable problems in deterministic stage order. */
        std::vector<Diagnostic> diagnostics;
};

/** @brief Reports validated logical and physical index totals. */
struct IndexStats {
        /** Sum of active physical Segment file sizes. */
        std::uint64_t bytes{};
        /** Visible live documents after path resolution. */
        std::uint64_t documents{};
        /** Active physical Segment count. */
        std::uint64_t segments{};
        /** Physical Tombstone records retained in active Segments. */
        std::uint64_t tombstones{};
        /** Terms remaining in the visible logical index. */
        std::uint64_t terms{};
        /** Postings remaining after visibility filtering. */
        std::uint64_t postings{};
        /** Positions belonging to retained Postings. */
        std::uint64_t positions{};
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
        std::filesystem::path index_directory_;
        IndexOptions options_;
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
