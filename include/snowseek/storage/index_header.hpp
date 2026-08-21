#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>

namespace snowseek::storage {

inline constexpr std::array<char, 8> kIndexMagic{'S', 'N', 'O', 'W',
                                                 'S', 'E', 'E', 'K'};
inline constexpr std::uint32_t kIndexFormatVersion = 1;

struct IndexHeader {
        std::uint32_t version = kIndexFormatVersion;
        std::uint32_t feature_flags{};
};

/**
 * @brief Writes a SnowSeek index header in its fixed little-endian format.
 * @param output Destination stream positioned at the header location.
 * @param header Version and feature flags to serialize.
 * @throws std::runtime_error If the stream cannot write the complete header.
 */
void write_header(std::ostream &output, const IndexHeader &header);

/**
 * @brief Reads and validates a SnowSeek index header.
 * @param input Source stream positioned at the header location.
 * @return The decoded format version and feature flags.
 * @throws std::runtime_error If the header is truncated or has invalid magic.
 */
[[nodiscard]] IndexHeader read_header(std::istream &input);

} // namespace snowseek::storage
