#pragma once

#include "document/document_store.hpp"
#include "index/index.hpp"

#include <cstdint>
#include <filesystem>

namespace snowseek::storage {

inline constexpr const char *kSegmentFileName =
        "segment-0000000000000001.idx";

/** @brief Physical record totals for exactly one immutable Segment v2 file. */
struct SegmentStats {
        /** Exact serialized Segment byte size. */
        std::uint64_t file_size{};
        /** All Document records, including live records and Tombstones. */
        std::uint64_t physical_document_count{};
        /** Live Document records physically present in this Segment. */
        std::uint64_t live_document_count{};
        /** Tombstone records physically present in this Segment. */
        std::uint64_t tombstone_count{};
        /** Term records physically present in this Segment. */
        std::uint64_t term_count{};
        /** Posting records physically present in this Segment. */
        std::uint64_t posting_count{};
        /** Position records physically present in this Segment. */
        std::uint64_t position_count{};
};

/** @brief Resolved logical totals and retained physical costs for a directory. */
struct IndexStats {
        /** Sum of the active Segment byte sizes. */
        std::uint64_t file_size{};
        /** Live documents visible after newest-path resolution. */
        std::uint64_t live_document_count{};
        /** Tombstone records retained across all active Segments. */
        std::uint64_t tombstone_count{};
        /** All Document records retained across all active Segments. */
        std::uint64_t physical_document_count{};
        /** Number of active physical Segments. */
        std::uint64_t segment_count{};
        /** Terms remaining in the resolved logical index. */
        std::uint64_t term_count{};
        /** Postings remaining after newest-path filtering. */
        std::uint64_t posting_count{};
        /** Positions belonging to retained logical Postings. */
        std::uint64_t position_count{};
};

/** @brief Fully decoded documents, postings, and statistics for one Segment. */
struct LoadedSegment {
        document::DocumentStore documents;
        index::InMemoryIndex index;
        SegmentStats stats;
};

/** @brief Visible logical index resolved from one or more active Segments. */
struct LoadedIndex {
        document::DocumentStore documents;
        index::InMemoryIndex index;
        IndexStats stats;
};

/**
 * @brief Serializes a complete immutable in-memory index to one Segment file.
 * @param path Destination file path; its parent directory must exist.
 * @param documents Document metadata in contiguous DocumentId order.
 * @param index Positional inverted index whose postings satisfy ordering rules.
 * @return Physical statistics for the serialized Segment.
 * @throws std::runtime_error If data violates v2 limits or writing fails.
 */
[[nodiscard]] SegmentStats
write_index_file(const std::filesystem::path &path,
                 const document::DocumentStore &documents,
                 const index::InMemoryIndex &index);

/**
 * @brief Validates and loads a v2 Segment into query structures.
 * @param path Segment file to read.
 * @return Loaded documents, postings, and validated statistics.
 * @throws std::runtime_error If the file is inaccessible, malformed, or
 * corrupted.
 */
[[nodiscard]] LoadedSegment
read_index_file(const std::filesystem::path &path);

/**
 * @brief Validates a v2 Segment without constructing query structures.
 * @param path Segment file to inspect using bounded streaming buffers.
 * @return Physical statistics from the validated Segment records.
 * @throws std::runtime_error If the file is inaccessible, malformed, or
 * corrupted.
 */
[[nodiscard]] SegmentStats
validate_index_file(const std::filesystem::path &path);

/**
 * @brief Loads and resolves the active Segments selected by a directory.
 * @param directory Directory containing a Manifest v1 Segment set.
 * @return Visible live index data and aggregate physical statistics.
 * @throws std::runtime_error If MANIFEST or a selected Segment is invalid. A
 * legacy directory without MANIFEST is rejected with a rebuild diagnostic.
 */
[[nodiscard]] LoadedIndex
read_index_directory(const std::filesystem::path &directory);

/**
 * @brief Validates and resolves every active Segment in an index directory.
 * @param directory Directory containing a Manifest v1 Segment set.
 * @return Statistics for the resolved logical index and its physical Segments.
 * @throws std::runtime_error If directory metadata or Segment data is invalid.
 */
[[nodiscard]] IndexStats
validate_index_directory(const std::filesystem::path &directory);

} // namespace snowseek::storage
