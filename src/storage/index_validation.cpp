/**
 * @file index_validation.cpp
 * @brief Validates Segment contents and materializes trusted index data.
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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowseek::storage {
namespace {

constexpr std::size_t kChecksumBufferSize = 64 * 1024;

using common::detail::checked_add;
using common::detail::checked_multiply;
using detail::open_input;
using detail::read_exact;
using detail::seek_to;

/**
 * @brief Checks one section checksum with a fixed-size read buffer.
 * @param input Segment stream used for sequential section reads.
 * @param descriptor Validated section descriptor.
 * @throws std::runtime_error If reading fails or the checksum differs.
 */
void validate_checksum(std::istream &input,
                       const SectionDescriptor &descriptor) {
        seek_to(input, descriptor.offset);
        Crc32c checksum;
        std::array<char, kChecksumBufferSize> buffer{};
        std::uint64_t remaining = descriptor.length;
        while (remaining != 0) {
                const auto chunk = static_cast<std::size_t>(
                        std::min<std::uint64_t>(remaining, buffer.size()));
                read_exact(input, buffer.data(), chunk);
                checksum.update(std::string_view(buffer.data(), chunk));
                remaining -= chunk;
        }
        if (checksum.value() != descriptor.checksum) {
                throw std::runtime_error("index section checksum mismatch");
        }
}

/**
 * @brief Reads a v2 header and validates the complete physical file envelope.
 * @param path Segment path to inspect.
 * @return Validated header and canonical section directory.
 * @throws std::runtime_error If the file, header, size, or any section checksum
 * is invalid.
 */
[[nodiscard]] IndexHeader
read_validated_header(const std::filesystem::path &path) {
        auto input = open_input(path);
        const auto header = read_header(input);

        std::error_code size_error;
        const auto physical_size = std::filesystem::file_size(path, size_error);
        if (size_error || physical_size != header.file_size) {
                throw std::runtime_error(
                        "index file size does not match header");
        }
        for (const auto kind : kIndexSectionOrder) {
                validate_checksum(input, detail::section(header, kind));
        }
        return header;
}

/**
 * @brief Decodes validated generic UTF-8 bytes into a filesystem path.
 * @param bytes Nonempty canonical UTF-8 path bytes.
 * @return A path using the platform filesystem representation.
 */
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view bytes) {
        std::u8string encoded;
        encoded.assign(reinterpret_cast<const char8_t *>(bytes.data()),
                       bytes.size());
        return std::filesystem::path(encoded);
}

/**
 * @brief Reports whether a decoded path is normalized and source-relative.
 * @param path Decoded document path.
 * @return True only for a nonempty normalized relative path.
 */
[[nodiscard]] bool
is_relative_document_path(const std::filesystem::path &path) {
        return !path.empty() && !path.is_absolute() &&
               std::none_of(path.begin(), path.end(), [](const auto &component) {
                       return component == "." || component == "..";
               });
}

/**
 * @brief Restores a signed timestamp from its serialized bit pattern.
 * @param bits Stable two's-complement timestamp bits.
 * @return Signed Unix Epoch nanoseconds.
 */
[[nodiscard]] std::int64_t timestamp_from_bits(std::uint64_t bits) noexcept {
        std::int64_t value{};
        std::memcpy(&value, &bits, sizeof(value));
        return value;
}

/**
 * @brief Decodes and validates the Documents and Paths sections of a Segment.
 * @param path Segment path opened independently for both sections.
 * @param header Validated v2 header for path.
 * @return A real document-table result retaining token counts for posting
 * validation.
 * @throws std::runtime_error If a record, path slice, or metadata invariant is
 * invalid.
 */
[[nodiscard]] detail::LoadedDocumentTable
decode_document_table(const std::filesystem::path &path,
                      const IndexHeader &header) {
        const auto &documents_section =
                detail::section(header, SectionKind::documents);
        const auto &paths_section = detail::section(header, SectionKind::paths);
        auto documents_input = open_input(path);
        auto paths_input = open_input(path);
        seek_to(documents_input, documents_section.offset);
        const auto document_count = read_u64_le(documents_input);
        const auto expected_size = checked_add(
                8,
                checked_multiply(document_count, detail::kDocumentRecordSize,
                                 "documents section length"),
                "documents section length");
        if (documents_section.length != expected_size) {
                throw std::runtime_error(
                        "documents section has inconsistent length");
        }
        if (document_count > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("document count exceeds size_t");
        }

        detail::LoadedDocumentTable loaded;
        loaded.stores_positions =
                (header.feature_flags & kFeaturePositions) != 0;
        loaded.stats = SegmentStats{
                .file_size = header.file_size,
                .physical_document_count = document_count,
                .live_document_count = 0,
                .tombstone_count = 0,
                .term_count = 0,
                .posting_count = 0,
                .position_count = 0,
        };
        loaded.token_counts.reserve(static_cast<std::size_t>(document_count));

        std::uint64_t expected_path_offset = 0;
        for (std::uint64_t document_index = 0;
             document_index < document_count; ++document_index) {
                const auto record =
                        detail::read_document_record(documents_input);
                if (record.document_id != document_index ||
                    record.reserved != 0 ||
                    (record.flags & ~detail::kSupportedDocumentFlags) != 0) {
                        throw std::runtime_error(
                                "invalid document id, flags, or reserved field");
                }

                const bool tombstone =
                        (record.flags & detail::kDocumentTombstone) != 0;
                const bool has_content_crc32c =
                        (record.flags & detail::kDocumentContentCrc32c) != 0;
                if ((tombstone &&
                     (record.file_size != 0 ||
                      record.modified_time_bits != 0 ||
                      record.token_count != 0 || has_content_crc32c ||
                      record.content_crc32c != 0)) ||
                    (!has_content_crc32c && record.content_crc32c != 0)) {
                        throw std::runtime_error(
                                "invalid Tombstone or content checksum metadata");
                }
                if (record.path_length == 0 ||
                    record.path_offset != expected_path_offset ||
                    record.path_offset > paths_section.length ||
                    record.path_length >
                            paths_section.length - record.path_offset) {
                        throw std::runtime_error(
                                "document path exceeds Paths section");
                }

                std::string path_bytes(record.path_length, '\0');
                seek_to(paths_input,
                        checked_add(paths_section.offset, record.path_offset,
                                    "path offset"));
                read_exact(paths_input, path_bytes.data(), path_bytes.size());
                if (!detail::is_valid_utf8(path_bytes)) {
                        throw std::runtime_error(
                                "stored document path is not valid nonempty UTF-8");
                }
                const auto decoded_path = path_from_utf8(path_bytes);
                if (!is_relative_document_path(decoded_path)) {
                        throw std::runtime_error(
                                "stored document path is not normalized and relative");
                }

                const auto id = tombstone
                                        ? loaded.documents.add_tombstone(
                                                  decoded_path)
                                        : loaded.documents.add(
                                                  decoded_path,
                                                  record.file_size,
                                                  timestamp_from_bits(
                                                          record.modified_time_bits),
                                                  has_content_crc32c
                                                          ? std::optional<
                                                                    std::uint32_t>(
                                                                    record.content_crc32c)
                                                          : std::nullopt);
                if (tombstone) {
                        ++loaded.stats.tombstone_count;
                } else {
                        loaded.documents.set_token_count(id,
                                                         record.token_count);
                        ++loaded.stats.live_document_count;
                }
                loaded.token_counts.push_back(record.token_count);
                expected_path_offset = checked_add(
                        expected_path_offset, record.path_length,
                        "path byte range");
        }
        if (expected_path_offset != paths_section.length) {
                throw std::runtime_error(
                        "Paths section contains unreferenced trailing bytes");
        }
        return loaded;
}

/**
 * @brief Decodes and validates Terms, Postings, and Positions sections.
 * @tparam Consumer Callable accepting term bytes, a validated posting record,
 * and decoded positions.
 * @param path Segment path to decode.
 * @param header Validated v2 header for path.
 * @param token_counts Per-document totals from the validated Documents table.
 * @param stats Document statistics to extend with index record counts.
 * @param consume Called once for each valid posting.
 * @return Complete physical statistics for the Segment.
 * @throws std::runtime_error If ordering, ranges, positions, or token totals are
 * invalid.
 */
template <typename Consumer>
[[nodiscard]] SegmentStats decode_terms_and_postings(
        const std::filesystem::path &path, const IndexHeader &header,
        std::vector<std::uint64_t> token_counts, SegmentStats stats,
        Consumer consume) {
        const auto &terms_section =
                detail::section(header, SectionKind::terms);
        const auto &postings_section =
                detail::section(header, SectionKind::postings);
        const auto &positions_section =
                detail::section(header, SectionKind::positions);
        const bool has_positions =
                (header.feature_flags & kFeaturePositions) != 0;

        auto terms_input = open_input(path);
        auto term_bytes_input = open_input(path);
        auto postings_input = open_input(path);
        auto positions_input = open_input(path);
        seek_to(terms_input, terms_section.offset);
        const auto term_count = read_u64_le(terms_input);
        const auto term_bytes_begin = checked_add(
                8,
                checked_multiply(term_count, detail::kTermRecordSize,
                                 "term table length"),
                "term byte offset");
        if (term_bytes_begin > terms_section.length) {
                throw std::runtime_error("term table exceeds Terms section");
        }
        seek_to(postings_input, postings_section.offset);
        seek_to(positions_input, positions_section.offset);

        stats.term_count = term_count;
        std::uint64_t expected_term_offset = term_bytes_begin;
        std::uint64_t expected_posting_offset = 0;
        std::uint64_t expected_position_offset = 0;
        std::string previous_term;
        for (std::uint64_t term_index = 0; term_index < term_count;
             ++term_index) {
                const auto term_record =
                        detail::read_term_record(terms_input);
                if (term_record.term_length == 0 ||
                    term_record.term_offset != expected_term_offset ||
                    term_record.term_offset > terms_section.length ||
                    term_record.term_length >
                            terms_section.length - term_record.term_offset) {
                        throw std::runtime_error("invalid term byte range");
                }
                std::string term(term_record.term_length, '\0');
                seek_to(term_bytes_input,
                        checked_add(terms_section.offset,
                                    term_record.term_offset, "term offset"));
                read_exact(term_bytes_input, term.data(), term.size());
                if ((!previous_term.empty() && term <= previous_term) ||
                    !std::all_of(term.begin(), term.end(),
                                 [](unsigned char byte) {
                                         return byte < 0x80U;
                                 })) {
                        throw std::runtime_error(
                                "terms are not strictly sorted ASCII bytes");
                }
                previous_term = term;
                expected_term_offset = checked_add(
                        expected_term_offset, term_record.term_length,
                        "term byte range");

                if (term_record.document_frequency == 0 ||
                    term_record.posting_offset != expected_posting_offset ||
                    term_record.posting_length !=
                            checked_multiply(
                                    term_record.document_frequency,
                                    detail::kPostingRecordSize,
                                    "posting length") ||
                    term_record.posting_offset > postings_section.length ||
                    term_record.posting_length >
                            postings_section.length -
                                    term_record.posting_offset) {
                        throw std::runtime_error("invalid posting range");
                }

                std::uint32_t previous_document = 0;
                bool first_document = true;
                for (std::uint32_t posting_index = 0;
                     posting_index < term_record.document_frequency;
                     ++posting_index) {
                        const auto posting =
                                detail::read_posting_record(postings_input);
                        if (posting.document_id >= token_counts.size() ||
                            posting.frequency == 0 ||
                            (!first_document &&
                             posting.document_id <= previous_document) ||
                            (has_positions &&
                             posting.position_offset !=
                                     expected_position_offset) ||
                            (!has_positions && posting.position_offset != 0)) {
                                throw std::runtime_error(
                                        "invalid posting record ordering");
                        }
                        first_document = false;
                        previous_document = posting.document_id;

                        std::vector<index::Position> positions;
                        if (has_positions) {
                                positions.reserve(posting.frequency);
                        }
                        std::uint32_t previous_position = 0;
                        for (std::uint32_t position_index = 0;
                             has_positions &&
                             position_index < posting.frequency;
                             ++position_index) {
                                const auto position =
                                        read_u32_le(positions_input);
                                if (position_index != 0 &&
                                    position <= previous_position) {
                                        throw std::runtime_error(
                                                "posting positions are not increasing");
                                }
                                positions.push_back(position);
                                previous_position = position;
                        }

                        if (has_positions) {
                                expected_position_offset = checked_add(
                                        expected_position_offset,
                                        checked_multiply(
                                                posting.frequency,
                                                detail::kPositionRecordSize,
                                                "position range length"),
                                        "position offset");
                        }
                        if (expected_position_offset >
                            positions_section.length) {
                                throw std::runtime_error(
                                        "posting positions exceed section");
                        }
                        if (posting.frequency >
                            token_counts[posting.document_id]) {
                                throw std::runtime_error(
                                        "document token count does not match postings");
                        }
                        token_counts[posting.document_id] -= posting.frequency;
                        consume(term, posting, std::move(positions));
                        stats.posting_count = checked_add(
                                stats.posting_count, 1, "posting count");
                        if (has_positions) {
                                stats.position_count = checked_add(
                                        stats.position_count,
                                        posting.frequency, "position count");
                        }
                }
                expected_posting_offset = checked_add(
                        expected_posting_offset, term_record.posting_length,
                        "posting offset");
        }

        if (expected_term_offset != terms_section.length ||
            expected_posting_offset != postings_section.length ||
            expected_position_offset != positions_section.length) {
                throw std::runtime_error(
                        "index sections contain unreferenced trailing bytes");
        }
        for (const auto remaining : token_counts) {
                if (remaining != 0) {
                        throw std::runtime_error(
                                "document token count does not match postings");
                }
        }
        return stats;
}

/**
 * @brief Validates a Segment and loads both documents and query postings.
 * @param path Existing v2 Segment.
 * @return Fully loaded query structures and single-Segment statistics.
 * @throws std::runtime_error If any physical or logical invariant is invalid.
 */
[[nodiscard]] LoadedSegment load_segment(const std::filesystem::path &path) {
        const auto header = read_validated_header(path);
        auto documents = decode_document_table(path, header);
        LoadedSegment loaded;
        loaded.documents = std::move(documents.documents);
        loaded.index = index::InMemoryIndex(documents.stores_positions);
        const auto segment_stats = decode_terms_and_postings(
                path, header, std::move(documents.token_counts),
                documents.stats,
                [&loaded](const std::string &term,
                          const detail::PostingRecord &posting,
                          std::vector<index::Position> positions) {
                        loaded.index.add_posting(
                                term, posting.document_id, posting.frequency,
                                std::move(positions));
                });
        loaded.stats = segment_stats;
        return loaded;
}

} // namespace

SegmentStats validate_index_file(const std::filesystem::path &path) {
        const auto header = read_validated_header(path);
        auto documents = decode_document_table(path, header);
        const auto stats = decode_terms_and_postings(
                path, header, std::move(documents.token_counts),
                documents.stats,
                [](const std::string &, const detail::PostingRecord &,
                   std::vector<index::Position>) {});
        return stats;
}

LoadedSegment read_index_file(const std::filesystem::path &path) {
        return load_segment(path);
}

namespace detail {

LoadedDocumentTable
load_document_table(const std::filesystem::path &path) {
        const auto header = read_validated_header(path);
        auto documents = decode_document_table(path, header);
        documents.stats = decode_terms_and_postings(
                path, header, documents.token_counts, documents.stats,
                [](const std::string &, const PostingRecord &,
                   std::vector<index::Position>) {});
        return documents;
}

void append_remapped_postings(
        const std::filesystem::path &path,
        const LoadedDocumentTable &documents,
        const std::vector<std::uint64_t> &document_remap,
        index::InMemoryIndex &target) {
        if (document_remap.size() != documents.token_counts.size()) {
                throw std::runtime_error(
                        "document remap does not match Segment document count");
        }
        if (target.stores_positions() != documents.stores_positions) {
                throw std::runtime_error(
                        "active Segments have different position capabilities");
        }

        const auto header = read_validated_header(path);
        static_cast<void>(decode_terms_and_postings(
                path, header, documents.token_counts, documents.stats,
                [&document_remap, &target](
                        const std::string &term, const PostingRecord &posting,
                        std::vector<index::Position> positions) {
                        const auto mapped =
                                document_remap[posting.document_id];
                        if (mapped ==
                            std::numeric_limits<std::uint64_t>::max()) {
                                return;
                        }
                        if (mapped >
                            std::numeric_limits<document::DocumentId>::max()) {
                                throw std::runtime_error(
                                        "global document id exceeds uint32_t");
                        }
                        target.add_posting(
                                term,
                                static_cast<document::DocumentId>(mapped),
                                posting.frequency, std::move(positions));
                }));
}

} // namespace detail

} // namespace snowseek::storage
