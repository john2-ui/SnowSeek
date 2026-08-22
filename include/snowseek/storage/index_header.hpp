#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace snowseek::storage {

inline constexpr std::array<char, 8> kIndexMagic{'S', 'N', 'O', 'W',
                                                 'S', 'E', 'E', 'K'};
inline constexpr std::uint32_t kIndexFormatVersion = 1;
inline constexpr std::uint32_t kIndexHeaderSize = 200;
inline constexpr std::size_t kIndexSectionCount = 5;
inline constexpr std::uint32_t kFeaturePositions = 1U << 0U;
inline constexpr std::uint32_t kSupportedFeatureFlags = kFeaturePositions;

enum class SectionKind : std::uint32_t {
        documents = 1,
        paths = 2,
        terms = 3,
        postings = 4,
        positions = 5,
};

inline constexpr std::array<SectionKind, kIndexSectionCount> kIndexSectionOrder{
        SectionKind::documents, SectionKind::paths, SectionKind::terms,
        SectionKind::postings, SectionKind::positions};

struct SectionDescriptor {
        SectionKind kind{};
        std::uint64_t offset{};
        std::uint64_t length{};
        std::uint32_t checksum{};
};

struct IndexHeader {
        std::uint32_t version = kIndexFormatVersion;
        std::uint32_t feature_flags = kFeaturePositions;
        std::uint64_t file_size = kIndexHeaderSize;
        std::array<SectionDescriptor, kIndexSectionCount> sections{{
                {SectionKind::documents, kIndexHeaderSize, 0, 0},
                {SectionKind::paths, kIndexHeaderSize, 0, 0},
                {SectionKind::terms, kIndexHeaderSize, 0, 0},
                {SectionKind::postings, kIndexHeaderSize, 0, 0},
                {SectionKind::positions, kIndexHeaderSize, 0, 0},
        }};
};

/**
 * @brief Writes a SnowSeek index header in its fixed little-endian format.
 * @param output Destination stream positioned at the header location.
 * @param header Version, flags, file size, and ordered section directory.
 * @throws std::runtime_error If the header violates the v1 format invariants
 * or the stream cannot write all 200 bytes.
 */
void write_header(std::ostream &output, const IndexHeader &header);

/**
 * @brief Reads and validates a SnowSeek index header.
 * @param input Source stream positioned at the header location.
 * @return The decoded and validated v1 header and section directory.
 * @throws std::runtime_error If the header is truncated, corrupted, uses an
 * unsupported version or flag, or contains invalid section boundaries.
 */
[[nodiscard]] IndexHeader read_header(std::istream &input);

} // namespace snowseek::storage
