/**
 * @file index_file_internal.hpp
 * @brief Declares internal bounded Segment I/O and validation helpers.
 */

#pragma once

#include "storage/index_file.hpp"
#include "storage/index_header.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::storage::detail {

/** @brief A fully validated Segment document table without query postings. */
struct LoadedDocumentTable {
        document::DocumentStore documents; ///< Physical records in local ID order.
        std::vector<std::uint64_t> token_counts; ///< Counts indexed by local document ID.
        SegmentStats stats; ///< Validated physical Segment totals.
        IndexHeader header; ///< Validated envelope reused by the second decode pass.
        bool stores_positions{}; ///< Whether Posting offsets reference Position data.
};

/**
 * @brief Serializes one index after enforcing a maximum output file size.
 * @param path Destination Segment path whose parent already exists.
 * @param documents Documents in contiguous identifier order.
 * @param index Positional inverted index to encode.
 * @param maximum_file_size Maximum bytes the completed Segment may occupy.
 * @return Physical and logical statistics for the written Segment.
 * @throws std::runtime_error If encoding fails, the size limit or available
 * filesystem space is insufficient, or the output cannot be written.
 */
[[nodiscard]] SegmentStats
write_index_file_bounded(const std::filesystem::path &path,
                         const document::DocumentStore &documents,
                         const index::InMemoryIndex &index,
                         std::uint64_t maximum_file_size);

/**
 * @brief Validates a Segment and materializes its complete Document table.
 * @param path Existing v2 Segment.
 * @return A dedicated document-table result with physical Segment statistics.
 * @throws std::runtime_error If the Segment is inaccessible or malformed.
 */
[[nodiscard]] LoadedDocumentTable
load_document_table(const std::filesystem::path &path);

/**
 * @brief Appends visible remapped Postings from a validated Segment table.
 * @param path Existing v2 Segment matching documents.
 * @param documents Previously validated document-table metadata.
 * @param document_remap Local IDs mapped to global IDs; uint64 max drops one.
 * @param target Combined index receiving retained Postings.
 * @throws std::runtime_error If the posting records are malformed or the
 * Segment capabilities differ from target.
 */
void append_remapped_postings(
        const std::filesystem::path &path,
        const LoadedDocumentTable &documents,
        const std::vector<std::uint64_t> &document_remap,
        index::InMemoryIndex &target);

/**
 * @brief Opens an independent binary stream for Segment random access.
 * @param path Existing Segment or spool path.
 * @return An open input stream.
 * @throws std::runtime_error If the path cannot be opened.
 */
[[nodiscard]] inline std::ifstream
open_input(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
                throw std::runtime_error("failed to open index file: " +
                                         path.string());
        }
        return input;
}

/**
 * @brief Positions a stream at an absolute Segment offset.
 * @param input Stream to reposition.
 * @param offset Absolute byte offset from the file start.
 * @throws std::runtime_error If the offset is unsupported or seeking fails.
 */
inline void seek_to(std::istream &input, std::uint64_t offset) {
        if (offset > static_cast<std::uint64_t>(
                             std::numeric_limits<std::streamoff>::max())) {
                throw std::runtime_error("index offset exceeds stream limit");
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset));
        if (!input) {
                throw std::runtime_error("failed to seek index file");
        }
}

/**
 * @brief Reads an exact byte range from the current stream position.
 * @param input Stream supplying bytes.
 * @param data Destination buffer.
 * @param size Required byte count.
 * @throws std::runtime_error If the stream ends early or reports an error.
 */
inline void read_exact(std::istream &input, char *data, std::size_t size) {
        input.read(data, static_cast<std::streamsize>(size));
        if (!input || static_cast<std::size_t>(input.gcount()) != size) {
                throw std::runtime_error("index file is truncated");
        }
}

/**
 * @brief Validates one complete canonical UTF-8 byte sequence.
 * @param bytes Candidate UTF-8 bytes.
 * @return True only for canonical bytes without surrogates or overflow.
 */
[[nodiscard]] inline bool is_valid_utf8(std::string_view bytes) noexcept {
        std::size_t index = 0;
        while (index < bytes.size()) {
                const auto first = static_cast<unsigned char>(bytes[index]);
                std::size_t length = 0;
                if (first <= 0x7fU) {
                        length = 1;
                } else if (first >= 0xc2U && first <= 0xdfU) {
                        length = 2;
                } else if (first >= 0xe0U && first <= 0xefU) {
                        length = 3;
                } else if (first >= 0xf0U && first <= 0xf4U) {
                        length = 4;
                } else {
                        return false;
                }
                if (length > bytes.size() - index) {
                        return false;
                }
                for (std::size_t offset = 1; offset < length; ++offset) {
                        const auto byte = static_cast<unsigned char>(
                                bytes[index + offset]);
                        if (byte < 0x80U || byte > 0xbfU) {
                                return false;
                        }
                        if (offset == 1 && ((first == 0xe0U && byte < 0xa0U) ||
                                            (first == 0xedU && byte > 0x9fU) ||
                                            (first == 0xf0U && byte < 0x90U) ||
                                            (first == 0xf4U && byte > 0x8fU))) {
                                return false;
                        }
                }
                index += length;
        }
        return true;
}

} // namespace snowseek::storage::detail
