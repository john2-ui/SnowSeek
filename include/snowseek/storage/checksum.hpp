#pragma once

#include <cstdint>
#include <string_view>

namespace snowseek::storage {

/**
 * @brief Computes the CRC32C Castagnoli checksum of a byte range.
 * @param bytes Complete byte range to checksum, including embedded nulls.
 * @return CRC32C using an all-ones initial state and final XOR.
 */
[[nodiscard]] std::uint32_t crc32c(std::string_view bytes) noexcept;

} // namespace snowseek::storage
