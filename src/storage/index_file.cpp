#include "snowseek/storage/index_file.hpp"

#include "snowseek/storage/binary_codec.hpp"
#include "snowseek/storage/checksum.hpp"
#include "snowseek/storage/index_header.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
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

struct TermRecord {
        std::uint64_t term_offset{};
        std::uint32_t term_length{};
        std::uint32_t document_frequency{};
        std::uint64_t posting_offset{};
        std::uint64_t posting_length{};
};

/** @brief Validates one complete canonical UTF-8 byte sequence. */
[[nodiscard]] bool is_valid_utf8(std::string_view bytes) noexcept {
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
                        if (offset == 1 &&
                            ((first == 0xe0U && byte < 0xa0U) ||
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

/** @brief Encodes one path as portable generic UTF-8 bytes. */
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path) {
        const auto encoded = path.generic_u8string();
        std::string bytes(reinterpret_cast<const char *>(encoded.data()),
                          encoded.size());
        if (bytes.empty() || !is_valid_utf8(bytes)) {
                throw std::runtime_error(
                        "document path is not valid nonempty UTF-8");
        }
        return bytes;
}

/** @brief Decodes validated generic UTF-8 bytes into a filesystem path. */
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view bytes) {
        if (bytes.empty() || !is_valid_utf8(bytes)) {
                throw std::runtime_error(
                        "stored document path is not valid nonempty UTF-8");
        }
        std::u8string encoded;
        encoded.assign(reinterpret_cast<const char8_t *>(bytes.data()),
                       bytes.size());
        return std::filesystem::path(encoded);
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

/** @brief Adds two offsets without unsigned wraparound. */
[[nodiscard]] std::uint64_t checked_add(std::uint64_t left,
                                        std::uint64_t right,
                                        std::string_view field) {
        if (right > std::numeric_limits<std::uint64_t>::max() - left) {
                throw std::runtime_error(std::string(field) + " overflows");
        }
        return left + right;
}

/** @brief Multiplies two section sizes without unsigned wraparound. */
[[nodiscard]] std::uint64_t checked_multiply(std::uint64_t left,
                                             std::uint64_t right,
                                             std::string_view field) {
        if (left != 0 &&
            right > std::numeric_limits<std::uint64_t>::max() / left) {
                throw std::runtime_error(std::string(field) + " overflows");
        }
        return left * right;
}

/** @brief Converts a signed timestamp to its stable two's-complement bits. */
[[nodiscard]] std::uint64_t timestamp_bits(std::int64_t value) noexcept {
        std::uint64_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
}

/** @brief Restores a signed timestamp from its serialized bit pattern. */
[[nodiscard]] std::int64_t timestamp_from_bits(std::uint64_t bits) noexcept {
        std::int64_t value{};
        std::memcpy(&value, &bits, sizeof(value));
        return value;
}

/** @brief Returns complete stream bytes or reports an encoding failure. */
[[nodiscard]] std::string finish(std::ostringstream &stream) {
        if (!stream) {
                throw std::runtime_error("failed to encode index section");
        }
        return stream.str();
}

/** @brief Reads an entire Segment into memory for bounded random access. */
[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
                throw std::runtime_error("failed to open index file: " +
                                         path.string());
        }
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < 0) {
                throw std::runtime_error("failed to size index file: " +
                                         path.string());
        }
        const auto size = checked_narrow<std::size_t>(
                static_cast<std::uint64_t>(end), "index file size");
        std::string bytes(size, '\0');
        input.seekg(0);
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size()) {
                throw std::runtime_error("failed to read complete index file: " +
                                         path.string());
        }
        return bytes;
}

/** @brief Returns one validated section view from complete file bytes. */
[[nodiscard]] std::string_view
section_view(std::string_view bytes, const SectionDescriptor &section) {
        if (section.offset > bytes.size() ||
            section.length > bytes.size() - section.offset) {
                throw std::runtime_error("index section exceeds file bounds");
        }
        const auto view = bytes.substr(
                checked_narrow<std::size_t>(section.offset, "section offset"),
                checked_narrow<std::size_t>(section.length, "section length"));
        if (crc32c(view) != section.checksum) {
                throw std::runtime_error("index section checksum mismatch");
        }
        return view;
}

/** @brief Opens a bounded section stream over an immutable byte view. */
[[nodiscard]] std::istringstream section_stream(std::string_view bytes) {
        return std::istringstream(std::string(bytes),
                                  std::ios::in | std::ios::binary);
}

/** @brief Requires a fixed-size section to be consumed exactly. */
void require_size(std::string_view section, std::uint64_t expected,
                  std::string_view name) {
        if (section.size() != expected) {
                throw std::runtime_error(std::string(name) +
                                         " section has inconsistent length");
        }
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

LoadedIndex read_index_file(const std::filesystem::path &path) {
        const std::string bytes = read_file(path);
        if (bytes.size() < kIndexHeaderSize) {
                throw std::runtime_error("index file is truncated before header");
        }
        std::istringstream header_stream(
                bytes.substr(0, kIndexHeaderSize),
                std::ios::in | std::ios::binary);
        const auto header = read_header(header_stream);
        if (header.file_size != bytes.size()) {
                throw std::runtime_error("index file size does not match header");
        }

        // Validate every region checksum before interpreting logical records.
        std::array<std::string_view, kIndexSectionCount> sections{};
        for (std::size_t index_value = 0; index_value < sections.size();
             ++index_value) {
                sections[index_value] =
                        section_view(bytes, header.sections[index_value]);
        }

        LoadedIndex loaded;
        loaded.stats.file_size = bytes.size();

        // Decode document records and resolve their bounded path slices.
        auto document_stream = section_stream(sections[0]);
        const auto document_count = read_u64_le(document_stream);
        require_size(sections[0],
                     checked_add(8,
                                 checked_multiply(document_count,
                                                  kDocumentRecordSize,
                                                  "documents section length"),
                                 "documents section length"),
                     "documents");
        loaded.stats.document_count = document_count;
        std::uint64_t expected_path_offset = 0;
        for (std::uint64_t index_value = 0; index_value < document_count;
             ++index_value) {
                const auto document_id = read_u32_le(document_stream);
                const auto path_length = read_u32_le(document_stream);
                const auto path_offset = read_u64_le(document_stream);
                const auto file_size = read_u64_le(document_stream);
                const auto modified_time = read_u64_le(document_stream);
                const auto token_count = read_u32_le(document_stream);
                const auto reserved = read_u32_le(document_stream);
                if (document_id != index_value || reserved != 0) {
                        throw std::runtime_error(
                                "invalid document id or reserved field");
                }
                if (path_offset != expected_path_offset ||
                    path_offset > sections[1].size() ||
                    path_length > sections[1].size() - path_offset) {
                        throw std::runtime_error(
                                "document path exceeds Paths section");
                }
                const auto path_view = sections[1].substr(
                        checked_narrow<std::size_t>(path_offset, "path offset"),
                        path_length);
                const auto id = loaded.documents.add(
                        path_from_utf8(path_view), file_size,
                        timestamp_from_bits(modified_time));
                loaded.documents.set_token_count(id, token_count);
                expected_path_offset = checked_add(
                        expected_path_offset, path_length, "path byte range");
        }
        if (expected_path_offset != sections[1].size()) {
                throw std::runtime_error(
                        "Paths section contains unreferenced trailing bytes");
        }

        // Decode the sorted term table, then rebuild ordered postings from the
        // referenced Postings and Positions byte ranges.
        auto term_stream = section_stream(sections[2]);
        const auto term_count = read_u64_le(term_stream);
        const auto term_bytes_begin = checked_add(
                8, checked_multiply(term_count, kTermRecordSize,
                                    "term table length"),
                "term byte offset");
        if (term_bytes_begin > sections[2].size()) {
                throw std::runtime_error("term table exceeds Terms section");
        }
        std::vector<TermRecord> term_records;
        term_records.reserve(
                checked_narrow<std::size_t>(term_count, "term count"));
        for (std::uint64_t index_value = 0; index_value < term_count;
             ++index_value) {
                term_records.push_back(TermRecord{
                        read_u64_le(term_stream), read_u32_le(term_stream),
                        read_u32_le(term_stream), read_u64_le(term_stream),
                        read_u64_le(term_stream)});
        }
        loaded.stats.term_count = term_count;
        std::uint64_t expected_term_offset = term_bytes_begin;
        std::uint64_t expected_posting_offset = 0;
        std::uint64_t expected_position_offset = 0;
        std::string previous_term;
        std::vector<std::uint64_t> observed_token_counts(
                checked_narrow<std::size_t>(document_count,
                                            "document count"));
        for (const auto &record : term_records) {
                if (record.term_length == 0 ||
                    record.term_offset != expected_term_offset ||
                    record.term_offset > sections[2].size() ||
                    record.term_length > sections[2].size() - record.term_offset) {
                        throw std::runtime_error("invalid term byte range");
                }
                const std::string term(sections[2].substr(
                        checked_narrow<std::size_t>(record.term_offset,
                                                    "term offset"),
                        record.term_length));
                if ((!previous_term.empty() && term <= previous_term) ||
                    !std::all_of(term.begin(), term.end(), [](unsigned char ch) {
                            return ch < 0x80U;
                    })) {
                        throw std::runtime_error(
                                "terms are not strictly sorted ASCII bytes");
                }
                previous_term = term;
                expected_term_offset = checked_add(
                        expected_term_offset, record.term_length,
                        "term byte range");
                if (record.document_frequency == 0 ||
                    record.posting_offset != expected_posting_offset ||
                    record.posting_length != checked_multiply(
                                                     record.document_frequency,
                                                     kPostingRecordSize,
                                                     "posting length") ||
                    record.posting_offset > sections[3].size() ||
                    record.posting_length >
                            sections[3].size() - record.posting_offset) {
                        throw std::runtime_error("invalid posting range");
                }
                auto posting_stream = section_stream(sections[3].substr(
                        checked_narrow<std::size_t>(record.posting_offset,
                                                    "posting offset"),
                        checked_narrow<std::size_t>(record.posting_length,
                                                    "posting length")));
                document::DocumentId previous_document{};
                bool first_document = true;
                for (std::uint32_t posting_index = 0;
                     posting_index < record.document_frequency;
                     ++posting_index) {
                        const auto document_id = read_u32_le(posting_stream);
                        const auto frequency = read_u32_le(posting_stream);
                        const auto position_offset = read_u64_le(posting_stream);
                        if (document_id >= document_count || frequency == 0 ||
                            (!first_document &&
                             document_id <= previous_document) ||
                            position_offset != expected_position_offset) {
                                throw std::runtime_error(
                                        "invalid posting record ordering");
                        }
                        first_document = false;
                        previous_document = document_id;
                        const auto position_length = checked_multiply(
                                frequency, 4, "position range length");
                        if (position_offset > sections[4].size() ||
                            position_length >
                                    sections[4].size() - position_offset) {
                                throw std::runtime_error(
                                        "posting positions exceed section");
                        }
                        auto position_stream = section_stream(
                                sections[4].substr(
                                        checked_narrow<std::size_t>(
                                                position_offset,
                                                "position offset"),
                                        checked_narrow<std::size_t>(
                                                position_length,
                                                "position length")));
                        index::Position previous_position{};
                        for (std::uint32_t position_index = 0;
                             position_index < frequency; ++position_index) {
                                const auto position =
                                        read_u32_le(position_stream);
                                if (position_index != 0 &&
                                    position <= previous_position) {
                                        throw std::runtime_error(
                                                "posting positions are not increasing");
                                }
                                loaded.index.add_occurrence(term, document_id,
                                                            position);
                                previous_position = position;
                        }
                        expected_position_offset = checked_add(
                                expected_position_offset, position_length,
                                "position offset");
                        observed_token_counts[document_id] = checked_add(
                                observed_token_counts[document_id], frequency,
                                "document token count");
                        ++loaded.stats.posting_count;
                        loaded.stats.position_count = checked_add(
                                loaded.stats.position_count, frequency,
                                "position count");
                }
                expected_posting_offset = checked_add(
                        expected_posting_offset, record.posting_length,
                        "posting offset");
        }
        if (expected_term_offset != sections[2].size() ||
            expected_posting_offset != sections[3].size() ||
            expected_position_offset != sections[4].size()) {
                throw std::runtime_error(
                        "index sections contain unreferenced trailing bytes");
        }
        for (std::size_t index_value = 0;
             index_value < observed_token_counts.size(); ++index_value) {
                if (observed_token_counts[index_value] !=
                    loaded.documents.get(
                            static_cast<document::DocumentId>(index_value))
                            .token_count) {
                        throw std::runtime_error(
                                "document token count does not match postings");
                }
        }
        return loaded;
}

} // namespace snowseek::storage
