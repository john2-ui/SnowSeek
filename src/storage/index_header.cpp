/**
 * @file index_header.cpp
 * @brief Encodes and validates the fixed checksummed Segment header.
 */

#include "storage/index_header.hpp"

#include "storage/binary_codec.hpp"
#include "storage/checksum.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace snowseek::storage {
namespace {

constexpr std::size_t kChecksummedHeaderSize = 192;
constexpr std::uint32_t kSerializedSectionDescriptorSize = 32;
static_assert(32 + kIndexSectionCount * kSerializedSectionDescriptorSize ==
              kChecksummedHeaderSize);

/**
 * @brief Validates semantic and range invariants represented by IndexHeader.
 * @param header Header to validate before writing or after decoding.
 * @throws std::runtime_error If a version, flag, section, or boundary is not
 * valid for the packed Segment layout.
 */
void validate_header(const IndexHeader &header) {
        if (header.version == kLegacyIndexFormatVersion) {
                throw std::runtime_error(
                        "Segment v1 is no longer supported; rebuild the index");
        }
        if (header.version != kIndexFormatVersion) {
                throw std::runtime_error("unsupported index format version");
        }
        if ((header.feature_flags & ~kSupportedFeatureFlags) != 0) {
                throw std::runtime_error("unsupported index feature flags");
        }
        if (header.file_size < kIndexHeaderSize) {
                throw std::runtime_error("index file is smaller than header");
        }

        std::uint64_t expected_offset = kIndexHeaderSize;
        for (std::size_t index = 0; index < header.sections.size(); ++index) {
                const auto &section = header.sections[index];
                if (section.kind != kIndexSectionOrder[index]) {
                        throw std::runtime_error(
                                "index sections are missing or out of order");
                }
                if (section.offset != expected_offset) {
                        throw std::runtime_error(
                                "index sections are not packed contiguously");
                }
                if (section.length > std::numeric_limits<std::uint64_t>::max() -
                                             section.offset) {
                        throw std::runtime_error(
                                "index section boundary overflows uint64_t");
                }
                if (section.length == 0 && section.checksum != 0) {
                        throw std::runtime_error(
                                "empty index section has a nonzero checksum");
                }
                expected_offset = section.offset + section.length;
        }

        if ((header.feature_flags & kFeaturePositions) == 0 &&
            header.sections.back().length != 0) {
                throw std::runtime_error(
                        "positions section requires the positions feature");
        }
        if (expected_offset != header.file_size) {
                throw std::runtime_error(
                        "index file size does not match section directory");
        }
}

/**
 * @brief Serializes the checksummed 192-byte Segment header prefix.
 * @param header Validated header whose fixed fields are encoded.
 * @return Exactly 192 bytes ending with the fifth section descriptor.
 * @throws std::runtime_error If an in-memory stream write fails.
 */
[[nodiscard]] std::string
serialize_checksummed_prefix(const IndexHeader &header) {
        std::ostringstream encoded(std::ios::out | std::ios::binary);
        encoded.write(kIndexMagic.data(),
                      static_cast<std::streamsize>(kIndexMagic.size()));
        write_u32_le(encoded, header.version);
        write_u32_le(encoded, header.feature_flags);
        write_u32_le(encoded, kIndexHeaderSize);
        write_u32_le(encoded, static_cast<std::uint32_t>(kIndexSectionCount));
        write_u64_le(encoded, header.file_size);

        for (const auto &section : header.sections) {
                write_u32_le(encoded, static_cast<std::uint32_t>(section.kind));
                write_u32_le(encoded, 0);
                write_u64_le(encoded, section.offset);
                write_u64_le(encoded, section.length);
                write_u32_le(encoded, section.checksum);
                write_u32_le(encoded, 0);
        }

        auto bytes = encoded.str();
        if (bytes.size() != kChecksummedHeaderSize) {
                throw std::runtime_error("internal index header size mismatch");
        }
        return bytes;
}

} // namespace

void write_header(std::ostream &output, const IndexHeader &header) {
        validate_header(header);
        const auto prefix = serialize_checksummed_prefix(header);
        output.write(prefix.data(),
                     static_cast<std::streamsize>(prefix.size()));
        write_u32_le(output, crc32c(prefix));
        write_u32_le(output, 0);
        if (!output) {
                throw std::runtime_error("failed to write index header");
        }
}

IndexHeader read_header(std::istream &input) {
        std::string bytes(kIndexHeaderSize, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
                throw std::runtime_error("truncated index header");
        }

        std::istringstream encoded(bytes, std::ios::in | std::ios::binary);
        std::array<char, kIndexMagic.size()> magic{};
        encoded.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!encoded || magic != kIndexMagic) {
                throw std::runtime_error("invalid SnowSeek index magic");
        }

        IndexHeader header;
        header.version = read_u32_le(encoded);
        header.feature_flags = read_u32_le(encoded);
        const auto header_size = read_u32_le(encoded);
        const auto section_count = read_u32_le(encoded);
        header.file_size = read_u64_le(encoded);

        if (header_size != kIndexHeaderSize) {
                throw std::runtime_error("unsupported index header size");
        }
        if (section_count != kIndexSectionCount) {
                throw std::runtime_error("unsupported index section count");
        }

        for (auto &section : header.sections) {
                section.kind = static_cast<SectionKind>(read_u32_le(encoded));
                const auto section_flags = read_u32_le(encoded);
                section.offset = read_u64_le(encoded);
                section.length = read_u64_le(encoded);
                section.checksum = read_u32_le(encoded);
                const auto section_reserved = read_u32_le(encoded);
                if (section_flags != 0 || section_reserved != 0) {
                        throw std::runtime_error(
                                "index section reserved field is nonzero");
                }
        }

        const auto stored_checksum = read_u32_le(encoded);
        const auto header_reserved = read_u32_le(encoded);
        if (header_reserved != 0) {
                throw std::runtime_error(
                        "index header reserved field is nonzero");
        }
        const auto prefix = bytes.substr(0, kChecksummedHeaderSize);
        if (stored_checksum != crc32c(prefix)) {
                throw std::runtime_error("index header checksum mismatch");
        }

        validate_header(header);
        return header;
}

} // namespace snowseek::storage
