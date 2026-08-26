#pragma once

#include "storage/binary_codec.hpp"
#include "storage/index_header.hpp"

#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace snowseek::storage::detail {

inline constexpr std::uint64_t kDocumentRecordSize = 48;
inline constexpr std::uint64_t kTermRecordSize = 32;
inline constexpr std::uint64_t kPostingRecordSize = 16;
inline constexpr std::uint64_t kPositionRecordSize = 4;
inline constexpr std::uint32_t kDocumentTombstone = 1U << 0U;
inline constexpr std::uint32_t kDocumentContentCrc32c = 1U << 1U;
inline constexpr std::uint32_t kSupportedDocumentFlags =
        kDocumentTombstone | kDocumentContentCrc32c;

struct DocumentRecord {
        std::uint32_t document_id{};
        std::uint32_t path_length{};
        std::uint64_t path_offset{};
        std::uint64_t file_size{};
        std::uint64_t modified_time_bits{};
        std::uint32_t token_count{};
        std::uint32_t flags{};
        std::uint32_t content_crc32c{};
        std::uint32_t reserved{};
};

struct TermRecord {
        std::uint64_t term_offset{};
        std::uint32_t term_length{};
        std::uint32_t document_frequency{};
        std::uint64_t posting_offset{};
        std::uint64_t posting_length{};
};

struct PostingRecord {
        std::uint32_t document_id{};
        std::uint32_t frequency{};
        std::uint64_t position_offset{};
};

/**
 * @brief Returns one descriptor from a validated canonical Segment header.
 * @param header Header whose section directory is indexed.
 * @param kind Required section kind.
 * @return The matching descriptor.
 * @throws std::logic_error If kind is outside the v2 section set or the header
 * is not in canonical order.
 */
[[nodiscard]] inline const SectionDescriptor &
section(const IndexHeader &header, SectionKind kind) {
        const auto value = static_cast<std::uint32_t>(kind);
        if (value == 0 || value > header.sections.size()) {
                throw std::logic_error("unknown Segment section kind");
        }
        const auto &descriptor = header.sections[value - 1];
        if (descriptor.kind != kind) {
                throw std::logic_error("Segment sections are not canonical");
        }
        return descriptor;
}

/**
 * @brief Returns one mutable descriptor from a canonical Segment header.
 * @param header Header whose section directory is indexed.
 * @param kind Required section kind.
 * @return The matching descriptor.
 * @throws std::logic_error If kind is outside the v2 section set or the header
 * is not in canonical order.
 */
[[nodiscard]] inline SectionDescriptor &section(IndexHeader &header,
                                                SectionKind kind) {
        const auto value = static_cast<std::uint32_t>(kind);
        if (value == 0 || value > header.sections.size()) {
                throw std::logic_error("unknown Segment section kind");
        }
        auto &descriptor = header.sections[value - 1];
        if (descriptor.kind != kind) {
                throw std::logic_error("Segment sections are not canonical");
        }
        return descriptor;
}

/**
 * @brief Encodes one fixed-width Segment v2 document record.
 * @param output Destination positioned inside the Documents section.
 * @param record Record fields to serialize in little-endian order.
 */
inline void write_document_record(std::ostream &output,
                                  const DocumentRecord &record) {
        write_u32_le(output, record.document_id);
        write_u32_le(output, record.path_length);
        write_u64_le(output, record.path_offset);
        write_u64_le(output, record.file_size);
        write_u64_le(output, record.modified_time_bits);
        write_u32_le(output, record.token_count);
        write_u32_le(output, record.flags);
        write_u32_le(output, record.content_crc32c);
        write_u32_le(output, record.reserved);
}

/**
 * @brief Decodes one fixed-width Segment v2 document record.
 * @param input Source positioned inside the Documents section.
 * @return Decoded record fields; semantic validation remains with the caller.
 * @throws std::runtime_error If the record is truncated.
 */
[[nodiscard]] inline DocumentRecord read_document_record(std::istream &input) {
        return DocumentRecord{read_u32_le(input), read_u32_le(input),
                              read_u64_le(input), read_u64_le(input),
                              read_u64_le(input), read_u32_le(input),
                              read_u32_le(input), read_u32_le(input),
                              read_u32_le(input)};
}

/**
 * @brief Encodes one fixed-width Segment v2 term record.
 * @param output Destination positioned in the Terms record table.
 * @param record Record fields to serialize in little-endian order.
 */
inline void write_term_record(std::ostream &output, const TermRecord &record) {
        write_u64_le(output, record.term_offset);
        write_u32_le(output, record.term_length);
        write_u32_le(output, record.document_frequency);
        write_u64_le(output, record.posting_offset);
        write_u64_le(output, record.posting_length);
}

/**
 * @brief Decodes one fixed-width Segment v2 term record.
 * @param input Source positioned in the Terms record table.
 * @return Decoded record fields; semantic validation remains with the caller.
 * @throws std::runtime_error If the record is truncated.
 */
[[nodiscard]] inline TermRecord read_term_record(std::istream &input) {
        return TermRecord{read_u64_le(input), read_u32_le(input),
                          read_u32_le(input), read_u64_le(input),
                          read_u64_le(input)};
}

/**
 * @brief Encodes one fixed-width Segment v2 posting record.
 * @param output Destination positioned in the Postings section.
 * @param record Record fields to serialize in little-endian order.
 */
inline void write_posting_record(std::ostream &output,
                                 const PostingRecord &record) {
        write_u32_le(output, record.document_id);
        write_u32_le(output, record.frequency);
        write_u64_le(output, record.position_offset);
}

/**
 * @brief Decodes one fixed-width Segment v2 posting record.
 * @param input Source positioned in the Postings section.
 * @return Decoded record fields; semantic validation remains with the caller.
 * @throws std::runtime_error If the record is truncated.
 */
[[nodiscard]] inline PostingRecord read_posting_record(std::istream &input) {
        return PostingRecord{read_u32_le(input), read_u32_le(input),
                             read_u64_le(input)};
}

} // namespace snowseek::storage::detail
