#include "snowseek/storage/index_file.hpp"

#include "snowseek/common/checked_arithmetic.hpp"
#include "snowseek/storage/binary_codec.hpp"
#include "snowseek/storage/checksum.hpp"
#include "snowseek/storage/index_header.hpp"
#include "storage/index_file_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowseek::storage {
namespace {

constexpr std::uint64_t kDocumentRecordSize = 40;
constexpr std::uint64_t kTermRecordSize = 32;
constexpr std::uint64_t kPostingRecordSize = 16;
constexpr std::size_t kChecksumBufferSize = 64 * 1024;

using common::detail::checked_add;
using common::detail::checked_multiply;
using detail::open_input;
using detail::read_exact;
using detail::seek_to;

/**
 * @brief Checks one section checksum with a fixed-size read buffer.
 * @param input Segment stream used for sequential section reads.
 * @param section Validated section descriptor.
 * @throws std::runtime_error If reading fails or the checksum differs.
 */
void validate_checksum(std::istream &input, const SectionDescriptor &section) {
        seek_to(input, section.offset);
        Crc32c checksum;
        std::array<char, kChecksumBufferSize> buffer{};
        std::uint64_t remaining = section.length;
        while (remaining != 0) {
                const auto chunk = static_cast<std::size_t>(
                        std::min<std::uint64_t>(remaining, buffer.size()));
                read_exact(input, buffer.data(), chunk);
                checksum.update(std::string_view(buffer.data(), chunk));
                remaining -= chunk;
        }
        if (checksum.value() != section.checksum) {
                throw std::runtime_error("index section checksum mismatch");
        }
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
 * @brief Streams, validates, and optionally materializes one v1 Segment.
 * @param path Segment path to parse.
 * @param documents Optional document table populated in serialized order.
 * @param loaded_index Optional inverted index populated from postings.
 * @return Validated logical and physical file statistics.
 * @throws std::runtime_error If the Segment is inaccessible or malformed.
 */
[[nodiscard]] IndexFileStats
parse_index_file(const std::filesystem::path &path,
                 document::DocumentStore *documents,
                 index::InMemoryIndex *loaded_index) {
        auto header_input = open_input(path);
        const auto header = read_header(header_input);
        const bool has_positions =
                (header.feature_flags & kFeaturePositions) != 0;
        if (loaded_index != nullptr) {
                *loaded_index = index::InMemoryIndex(has_positions);
        }
        std::error_code size_error;
        const auto file_size = std::filesystem::file_size(path, size_error);
        if (size_error || file_size != header.file_size) {
                throw std::runtime_error(
                        "index file size does not match header");
        }

        // Reject corruption before interpreting any logical record.
        for (const auto &section : header.sections) {
                validate_checksum(header_input, section);
        }

        auto documents_input = open_input(path);
        auto paths_input = open_input(path);
        seek_to(documents_input, header.sections[0].offset);
        const auto document_count = read_u64_le(documents_input);
        const auto expected_documents_size = checked_add(
                8,
                checked_multiply(document_count, kDocumentRecordSize,
                                 "documents section length"),
                "documents section length");
        if (header.sections[0].length != expected_documents_size) {
                throw std::runtime_error(
                        "documents section has inconsistent length");
        }

        // Validate contiguous document IDs and their packed path slices.
        std::vector<std::uint64_t> remaining_token_counts;
        if (document_count > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("document count exceeds size_t");
        }
        remaining_token_counts.reserve(
                static_cast<std::size_t>(document_count));
        std::uint64_t expected_path_offset = 0;
        for (std::uint64_t document_index = 0; document_index < document_count;
             ++document_index) {
                const auto document_id = read_u32_le(documents_input);
                const auto path_length = read_u32_le(documents_input);
                const auto path_offset = read_u64_le(documents_input);
                const auto file_size = read_u64_le(documents_input);
                const auto modified_time = read_u64_le(documents_input);
                const auto token_count = read_u32_le(documents_input);
                const auto reserved = read_u32_le(documents_input);
                if (document_id != document_index || reserved != 0) {
                        throw std::runtime_error(
                                "invalid document id or reserved field");
                }
                if (path_length == 0 || path_offset != expected_path_offset ||
                    path_offset > header.sections[1].length ||
                    path_length > header.sections[1].length - path_offset) {
                        throw std::runtime_error(
                                "document path exceeds Paths section");
                }
                std::string path_bytes(path_length, '\0');
                seek_to(paths_input, checked_add(header.sections[1].offset,
                                                 path_offset, "path offset"));
                read_exact(paths_input, path_bytes.data(), path_bytes.size());
                if (!detail::is_valid_utf8(path_bytes)) {
                        throw std::runtime_error("stored document path is not "
                                                 "valid nonempty UTF-8");
                }
                if (documents != nullptr) {
                        const auto id = documents->add(
                                path_from_utf8(path_bytes), file_size,
                                timestamp_from_bits(modified_time));
                        documents->set_token_count(id, token_count);
                }
                remaining_token_counts.push_back(token_count);
                expected_path_offset = checked_add(
                        expected_path_offset, path_length, "path byte range");
        }
        if (expected_path_offset != header.sections[1].length) {
                throw std::runtime_error(
                        "Paths section contains unreferenced trailing bytes");
        }

        auto terms_input = open_input(path);
        auto term_bytes_input = open_input(path);
        auto postings_input = open_input(path);
        auto positions_input = open_input(path);
        seek_to(terms_input, header.sections[2].offset);
        const auto term_count = read_u64_le(terms_input);
        const auto term_bytes_begin =
                checked_add(8,
                            checked_multiply(term_count, kTermRecordSize,
                                             "term table length"),
                            "term byte offset");
        if (term_bytes_begin > header.sections[2].length) {
                throw std::runtime_error("term table exceeds Terms section");
        }
        seek_to(postings_input, header.sections[3].offset);
        seek_to(positions_input, header.sections[4].offset);

        IndexFileStats stats{header.file_size, document_count, term_count, 0,
                             0};
        std::uint64_t expected_term_offset = term_bytes_begin;
        std::uint64_t expected_posting_offset = 0;
        std::uint64_t expected_position_offset = 0;
        std::string previous_term;
        for (std::uint64_t term_index = 0; term_index < term_count;
             ++term_index) {
                const auto term_offset = read_u64_le(terms_input);
                const auto term_length = read_u32_le(terms_input);
                const auto document_frequency = read_u32_le(terms_input);
                const auto posting_offset = read_u64_le(terms_input);
                const auto posting_length = read_u64_le(terms_input);
                if (term_length == 0 || term_offset != expected_term_offset ||
                    term_offset > header.sections[2].length ||
                    term_length > header.sections[2].length - term_offset) {
                        throw std::runtime_error("invalid term byte range");
                }
                std::string term(term_length, '\0');
                seek_to(term_bytes_input,
                        checked_add(header.sections[2].offset, term_offset,
                                    "term offset"));
                read_exact(term_bytes_input, term.data(), term.size());
                if ((!previous_term.empty() && term <= previous_term) ||
                    !std::all_of(
                            term.begin(), term.end(),
                            [](unsigned char byte) { return byte < 0x80U; })) {
                        throw std::runtime_error(
                                "terms are not strictly sorted ASCII bytes");
                }
                previous_term = std::move(term);
                expected_term_offset = checked_add(
                        expected_term_offset, term_length, "term byte range");

                if (document_frequency == 0 ||
                    posting_offset != expected_posting_offset ||
                    posting_length != checked_multiply(document_frequency,
                                                       kPostingRecordSize,
                                                       "posting length") ||
                    posting_offset > header.sections[3].length ||
                    posting_length >
                            header.sections[3].length - posting_offset) {
                        throw std::runtime_error("invalid posting range");
                }
                std::uint32_t previous_document = 0;
                bool first_document = true;
                for (std::uint32_t posting_index = 0;
                     posting_index < document_frequency; ++posting_index) {
                        const auto document_id = read_u32_le(postings_input);
                        const auto frequency = read_u32_le(postings_input);
                        const auto position_offset =
                                read_u64_le(postings_input);
                        if (document_id >= document_count || frequency == 0 ||
                            (!first_document &&
                             document_id <= previous_document) ||
                            (has_positions &&
                             position_offset != expected_position_offset) ||
                            (!has_positions && position_offset != 0)) {
                                throw std::runtime_error(
                                        "invalid posting record ordering");
                        }
                        first_document = false;
                        previous_document = document_id;
                        std::vector<index::Position> positions;
                        if (has_positions) {
                                positions.reserve(frequency);
                        }
                        std::uint32_t previous_position = 0;
                        for (std::uint32_t position_index = 0;
                             has_positions && position_index < frequency;
                             ++position_index) {
                                const auto position =
                                        read_u32_le(positions_input);
                                if (position_index != 0 &&
                                    position <= previous_position) {
                                        throw std::runtime_error(
                                                "posting positions are not "
                                                "increasing");
                                }
                                positions.push_back(position);
                                previous_position = position;
                        }
                        if (loaded_index != nullptr) {
                                loaded_index->add_posting(
                                        previous_term, document_id, frequency,
                                        std::move(positions));
                        }
                        if (has_positions) {
                                const auto position_length = checked_multiply(
                                        frequency, 4, "position range length");
                                expected_position_offset = checked_add(
                                        expected_position_offset,
                                        position_length, "position offset");
                        }
                        if (expected_position_offset >
                            header.sections[4].length) {
                                throw std::runtime_error(
                                        "posting positions exceed section");
                        }
                        if (frequency > remaining_token_counts[document_id]) {
                                throw std::runtime_error(
                                        "document token count does not match "
                                        "postings");
                        }
                        remaining_token_counts[document_id] -= frequency;
                        stats.posting_count = checked_add(stats.posting_count,
                                                          1, "posting count");
                        if (has_positions) {
                                stats.position_count = checked_add(
                                        stats.position_count, frequency,
                                        "position count");
                        }
                }
                expected_posting_offset =
                        checked_add(expected_posting_offset, posting_length,
                                    "posting offset");
        }
        if (expected_term_offset != header.sections[2].length ||
            expected_posting_offset != header.sections[3].length ||
            expected_position_offset != header.sections[4].length) {
                throw std::runtime_error(
                        "index sections contain unreferenced trailing bytes");
        }
        for (const auto remaining : remaining_token_counts) {
                if (remaining != 0) {
                        throw std::runtime_error(
                                "document token count does not match postings");
                }
        }
        return stats;
}

} // namespace

IndexFileStats validate_index_file(const std::filesystem::path &path) {
        return parse_index_file(path, nullptr, nullptr);
}

LoadedIndex read_index_file(const std::filesystem::path &path) {
        LoadedIndex loaded;
        loaded.stats = parse_index_file(path, &loaded.documents, &loaded.index);
        return loaded;
}

} // namespace snowseek::storage
