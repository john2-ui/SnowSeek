#include "snowseek/storage/index_file.hpp"

#include "snowseek/common/checked_arithmetic.hpp"
#include "snowseek/storage/binary_codec.hpp"
#include "snowseek/storage/checksum.hpp"
#include "snowseek/storage/index_header.hpp"
#include "storage/index_file_internal.hpp"

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

constexpr std::uint64_t kTermRecordSize = 32;
constexpr std::uint64_t kPostingRecordSize = 16;

struct TermRecord {
        std::uint64_t term_offset{};
        std::uint32_t term_length{};
        std::uint32_t document_frequency{};
        std::uint64_t posting_offset{};
        std::uint64_t posting_length{};
};

using common::detail::checked_add;
using common::detail::checked_multiply;

/** @brief Encodes one path as portable generic UTF-8 bytes. */
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path) {
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
        if (value > static_cast<Source>(
                            std::numeric_limits<Destination>::max())) {
                throw std::runtime_error(std::string(field) +
                                         " exceeds the v1 format limit");
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

IndexFileStats write_index_file(const std::filesystem::path &path,
                                const document::DocumentStore &documents,
                                const index::InMemoryIndex &index) {
        // Encode paths and their fixed document records in DocumentId order.
        std::ostringstream paths_stream(std::ios::out | std::ios::binary);
        std::ostringstream documents_stream(std::ios::out | std::ios::binary);
        write_u64_le(documents_stream, documents.size());
        std::uint64_t path_offset = 0;
        for (const auto &document : documents.all()) {
                const std::string path_bytes = path_to_utf8(document.path);
                const auto path_length = checked_narrow<std::uint32_t>(
                        path_bytes.size(), "path length");
                write_u32_le(documents_stream, document.id);
                write_u32_le(documents_stream, path_length);
                write_u64_le(documents_stream, path_offset);
                write_u64_le(documents_stream, document.file_size);
                write_u64_le(documents_stream,
                             timestamp_bits(document.modified_time_ns));
                write_u32_le(documents_stream, document.token_count);
                write_u32_le(documents_stream, 0);
                paths_stream.write(path_bytes.data(), path_bytes.size());
                path_offset = checked_add(path_offset, path_bytes.size(),
                                          "paths section length");
        }

        // Encode postings and positions in sorted term order, retaining records
        // needed to build the Terms section after final offsets are known.
        const auto terms = index.sorted_terms();
        std::ostringstream postings_stream(std::ios::out | std::ios::binary);
        std::ostringstream positions_stream(std::ios::out | std::ios::binary);
        std::vector<TermRecord> term_records;
        term_records.reserve(terms.size());
        std::uint64_t posting_offset = 0;
        std::uint64_t position_offset = 0;
        std::uint64_t posting_count = 0;
        std::uint64_t position_count = 0;
        for (const auto &term : terms) {
                const auto *postings = index.find(term);
                if (postings == nullptr || postings->empty()) {
                        throw std::runtime_error(
                                "index dictionary contains an empty posting list");
                }
                const auto document_frequency = checked_narrow<std::uint32_t>(
                        postings->size(), "document frequency");
                const auto posting_length = checked_multiply(
                        postings->size(), kPostingRecordSize,
                        "term posting length");
                term_records.push_back(TermRecord{
                        0, checked_narrow<std::uint32_t>(term.size(),
                                                        "term length"),
                        document_frequency, posting_offset, posting_length});
                for (const auto &posting : *postings) {
                        const auto frequency = posting.term_frequency();
                        if (frequency == 0) {
                                throw std::runtime_error(
                                        "posting term frequency is zero");
                        }
                        write_u32_le(postings_stream, posting.document_id);
                        write_u32_le(postings_stream, frequency);
                        write_u64_le(postings_stream, position_offset);
                        for (const auto position : posting.positions) {
                                write_u32_le(positions_stream, position);
                        }
                        position_offset = checked_add(
                                position_offset,
                                checked_multiply(frequency, 4,
                                                 "position byte length"),
                                "positions section length");
                        position_count = checked_add(position_count, frequency,
                                                     "position count");
                }
                posting_offset = checked_add(posting_offset, posting_length,
                                             "postings section length");
                posting_count = checked_add(posting_count, postings->size(),
                                            "posting count");
        }

        // The fixed term table precedes concatenated term bytes.
        std::ostringstream terms_stream(std::ios::out | std::ios::binary);
        write_u64_le(terms_stream, terms.size());
        std::uint64_t term_offset = checked_add(
                8, checked_multiply(terms.size(), kTermRecordSize,
                                    "term table length"),
                "term byte offset");
        for (std::size_t index_value = 0; index_value < terms.size();
             ++index_value) {
                auto &record = term_records[index_value];
                record.term_offset = term_offset;
                write_u64_le(terms_stream, record.term_offset);
                write_u32_le(terms_stream, record.term_length);
                write_u32_le(terms_stream, record.document_frequency);
                write_u64_le(terms_stream, record.posting_offset);
                write_u64_le(terms_stream, record.posting_length);
                term_offset = checked_add(term_offset, terms[index_value].size(),
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
        std::uint64_t offset = kIndexHeaderSize;
        for (std::size_t section_index = 0; section_index < sections.size();
             ++section_index) {
                auto &descriptor = header.sections[section_index];
                descriptor.offset = offset;
                descriptor.length = sections[section_index].size();
                descriptor.checksum = crc32c(sections[section_index]);
                offset = checked_add(offset, descriptor.length,
                                     "index file size");
        }
        header.file_size = offset;

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
        return IndexFileStats{header.file_size, documents.size(), terms.size(),
                              posting_count, position_count};
}


} // namespace snowseek::storage
