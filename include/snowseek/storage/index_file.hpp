#pragma once

#include "snowseek/document/document_store.hpp"
#include "snowseek/index/index.hpp"

#include <cstdint>
#include <filesystem>

namespace snowseek::storage {

inline constexpr const char *kSegmentFileName =
        "segment-0000000000000001.idx";

struct IndexFileStats {
        std::uint64_t file_size{};
        std::uint64_t document_count{};
        std::uint64_t term_count{};
        std::uint64_t posting_count{};
        std::uint64_t position_count{};
};

struct LoadedIndex {
        document::DocumentStore documents;
        index::InMemoryIndex index;
        IndexFileStats stats;
};

/**
 * @brief Serializes a complete immutable in-memory index to one Segment file.
 * @param path Destination file path; its parent directory must exist.
 * @param documents Document metadata in contiguous DocumentId order.
 * @param index Positional inverted index whose postings satisfy ordering rules.
 * @return Statistics describing the serialized file and logical records.
 * @throws std::runtime_error If data violates v1 limits or writing fails.
 */
[[nodiscard]] IndexFileStats
write_index_file(const std::filesystem::path &path,
                 const document::DocumentStore &documents,
                 const index::InMemoryIndex &index);

/**
 * @brief Validates and loads a v1 Segment into the in-memory query structures.
 * @param path Segment file to read.
 * @return Loaded documents, postings, and validated statistics.
 * @throws std::runtime_error If the file is inaccessible, malformed, or
 * corrupted.
 */
[[nodiscard]] LoadedIndex read_index_file(const std::filesystem::path &path);

} // namespace snowseek::storage
