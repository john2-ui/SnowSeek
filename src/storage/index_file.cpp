/**
 * @file index_file.cpp
 * @brief Serializes in-memory documents and postings into immutable Segments.
 */

#include "storage/index_file.hpp"

#include "common/checked_arithmetic.hpp"
#include "storage/binary_codec.hpp"
#include "storage/checksum.hpp"
#include "storage/index_header.hpp"
#include "storage/index_file_internal.hpp"
#include "storage/segment_format_internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::storage {
namespace {

using common::detail::checked_add;
using common::detail::checked_multiply;

/** @brief Encodes one path as portable generic UTF-8 bytes. */
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path) {
        if (path.empty() || path.is_absolute() ||
            std::any_of(path.begin(), path.end(), [](const auto &component) {
                    return component == "." || component == "..";
            })) {
                throw std::runtime_error(
                        "document path must be a normalized relative path");
        }
        const auto encoded = path.generic_u8string();
        std::string bytes(reinterpret_cast<const char *>(encoded.data()),
                          encoded.size());
        if (bytes.empty() || !detail::is_valid_utf8(bytes)) {
                throw std::runtime_error(
                        "document path is not valid nonempty UTF-8");
        }
        return bytes;
}

/** @brief Narrows a size after checking the destination range. */
template <typename Destination, typename Source>
[[nodiscard]] Destination checked_narrow(Source value, std::string_view field) {
        if (value >
            static_cast<Source>(std::numeric_limits<Destination>::max())) {
                throw std::runtime_error(std::string(field) +
                                         " exceeds the Segment format limit");
        }
        return static_cast<Destination>(value);
}

/** @brief Converts a signed timestamp to its stable two's-complement bits. */
[[nodiscard]] std::uint64_t timestamp_bits(std::int64_t value) noexcept {
        std::uint64_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
}

/** @brief Returns complete stream bytes or reports an encoding failure. */
[[nodiscard]] std::string finish(std::ostringstream &stream) {
        if (!stream) {
                throw std::runtime_error("failed to encode index section");
        }
        return stream.str();
}

} // namespace

SegmentStats
detail::write_index_file_bounded(const std::filesystem::path &path,
                                 const document::DocumentStore &documents,
                                 const index::InMemoryIndex &index,
                                 std::uint64_t maximum_file_size) {
        // Encode paths and their fixed document records in DocumentId order.
        std::ostringstream paths_stream(std::ios::out | std::ios::binary);
        std::ostringstream documents_stream(std::ios::out | std::ios::binary);
        write_u64_le(documents_stream, documents.size());
        std::uint64_t path_offset = 0;
        std::uint64_t live_document_count = 0;
        std::uint64_t tombstone_count = 0;
        for (const auto &document : documents.all()) {
                const std::string path_bytes = path_to_utf8(document.path);
                const auto path_length = checked_narrow<std::uint32_t>(
                        path_bytes.size(), "path length");
                std::uint32_t flags = 0;
                std::uint32_t content_crc32c = 0;
                if (document.state == document::DocumentState::tombstone) {
                        if (document.file_size != 0 ||
                            document.modified_time_ns != 0 ||
                            document.token_count != 0 ||
                            document.content_crc32c.has_value()) {
                                throw std::runtime_error(
                                        "Tombstone contains live document metadata");
                        }
                        flags = detail::kDocumentTombstone;
                        ++tombstone_count;
                } else {
                        ++live_document_count;
                        if (document.content_crc32c.has_value()) {
                                flags |= detail::kDocumentContentCrc32c;
                                content_crc32c = *document.content_crc32c;
                        }
                }
                detail::write_document_record(
                        documents_stream,
                        detail::DocumentRecord{
                                document.id, path_length, path_offset,
                                document.file_size,
                                timestamp_bits(document.modified_time_ns),
                                document.token_count, flags, content_crc32c, 0});
                paths_stream.write(path_bytes.data(), path_bytes.size());
                path_offset = checked_add(path_offset, path_bytes.size(),
                                          "paths section length");
        }

        // Encode postings and positions in sorted term order, retaining records
        // needed to build the Terms section after final offsets are known.
        const auto terms = index.sorted_terms();
        std::ostringstream postings_stream(std::ios::out | std::ios::binary);
        std::ostringstream positions_stream(std::ios::out | std::ios::binary);
        std::vector<detail::TermRecord> term_records;
        term_records.reserve(terms.size());
        std::uint64_t posting_offset = 0;
        std::uint64_t position_offset = 0;
        std::uint64_t posting_count = 0;
        std::uint64_t position_count = 0;
        for (const auto &term : terms) {
                const auto *postings = index.find(term);
                if (postings == nullptr || postings->empty()) {
                        throw std::runtime_error("index dictionary contains an "
                                                 "empty posting list");
                }
                const auto document_frequency = checked_narrow<std::uint32_t>(
                        postings->size(), "document frequency");
                const auto posting_length =
                        checked_multiply(postings->size(),
                                         detail::kPostingRecordSize,
                                         "term posting length");
                term_records.push_back(detail::TermRecord{
                        0,
                        checked_narrow<std::uint32_t>(term.size(),
                                                      "term length"),
                        document_frequency, posting_offset, posting_length});
                for (const auto &posting : *postings) {
                        if (posting.document_id >= documents.size() ||
                            documents.get(posting.document_id).state ==
                                    document::DocumentState::tombstone) {
                                throw std::runtime_error(
                                        "posting references a missing or Tombstone document");
                        }
                        const auto frequency = posting.term_frequency();
                        if (frequency == 0 ||
                            (index.stores_positions() &&
                             posting.positions.size() != frequency) ||
                            (!index.stores_positions() &&
                             !posting.positions.empty())) {
                                throw std::runtime_error(
                                        "posting frequency and positions "
                                        "mismatch");
                        }
                        detail::write_posting_record(
                                postings_stream,
                                detail::PostingRecord{posting.document_id,
                                                      frequency,
                                                      position_offset});
                        if (index.stores_positions()) {
                                for (const auto position : posting.positions) {
                                        write_u32_le(positions_stream,
                                                     position);
                                }
                                position_offset = checked_add(
                                        position_offset,
                                        checked_multiply(
                                                frequency,
                                                detail::kPositionRecordSize,
                                                "position byte length"),
                                        "positions section length");
                                position_count =
                                        checked_add(position_count, frequency,
                                                    "position count");
                        }
                }
                posting_offset = checked_add(posting_offset, posting_length,
                                             "postings section length");
                posting_count = checked_add(posting_count, postings->size(),
                                            "posting count");
        }

        // The fixed term table precedes concatenated term bytes.
        std::ostringstream terms_stream(std::ios::out | std::ios::binary);
        write_u64_le(terms_stream, terms.size());
        std::uint64_t term_offset =
                checked_add(8,
                            checked_multiply(terms.size(),
                                             detail::kTermRecordSize,
                                             "term table length"),
                            "term byte offset");
        for (std::size_t index_value = 0; index_value < terms.size();
             ++index_value) {
                auto &record = term_records[index_value];
                record.term_offset = term_offset;
                detail::write_term_record(terms_stream, record);
                term_offset =
                        checked_add(term_offset, terms[index_value].size(),
                                    "terms section length");
        }
        for (const auto &term : terms) {
                terms_stream.write(term.data(), term.size());
        }

        std::array<std::string, kIndexSectionCount> sections{
                finish(documents_stream), finish(paths_stream),
                finish(terms_stream), finish(postings_stream),
                finish(positions_stream)};
        IndexHeader header;
        header.feature_flags = index.stores_positions() ? kFeaturePositions : 0;
        std::uint64_t offset = kIndexHeaderSize;
        for (std::size_t section_index = 0; section_index < sections.size();
             ++section_index) {
                auto &descriptor = detail::section(
                        header, kIndexSectionOrder[section_index]);
                descriptor.offset = offset;
                descriptor.length = sections[section_index].size();
                descriptor.checksum = crc32c(sections[section_index]);
                offset = checked_add(offset, descriptor.length,
                                     "index file size");
        }
        header.file_size = offset;

        if (header.file_size > maximum_file_size) {
                throw std::runtime_error("temporary space budget exceeded "
                                         "while writing Segment");
        }
        std::error_code space_error;
        const auto parent = path.parent_path().empty()
                                    ? std::filesystem::path(".")
                                    : path.parent_path();
        const auto space = std::filesystem::space(parent, space_error);
        if (space_error) {
                throw std::runtime_error(
                        "failed to inspect temporary filesystem space: " +
                        space_error.message());
        }
        if (header.file_size > space.available) {
                throw std::runtime_error(
                        "insufficient temporary filesystem space");
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
                throw std::runtime_error("failed to create index file: " +
                                         path.string());
        }
        write_header(output, header);
        for (const auto &section : sections) {
                output.write(section.data(), section.size());
        }
        output.close();
        if (!output) {
                throw std::runtime_error("failed to write index file: " +
                                         path.string());
        }
        return SegmentStats{
                .file_size = header.file_size,
                .physical_document_count = documents.size(),
                .live_document_count = live_document_count,
                .tombstone_count = tombstone_count,
                .term_count = terms.size(),
                .posting_count = posting_count,
                .position_count = position_count,
        };
}

SegmentStats write_index_file(const std::filesystem::path &path,
                              const document::DocumentStore &documents,
                              const index::InMemoryIndex &index) {
        return detail::write_index_file_bounded(
                path, documents, index,
                std::numeric_limits<std::uint64_t>::max());
}

} // namespace snowseek::storage
