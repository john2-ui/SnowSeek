#pragma once

#include <cstdint>
#include <iosfwd>

namespace snowseek::storage {

/**
 * @brief Writes an unsigned 32-bit integer in little-endian byte order.
 * @param output Destination stream positioned at the write location.
 * @param value Integer to encode.
 * @throws std::runtime_error If all four bytes cannot be written.
 */
void write_u32_le(std::ostream &output, std::uint32_t value);

/**
 * @brief Writes an unsigned 64-bit integer in little-endian byte order.
 * @param output Destination stream positioned at the write location.
 * @param value Integer to encode.
 * @throws std::runtime_error If all eight bytes cannot be written.
 */
void write_u64_le(std::ostream &output, std::uint64_t value);

/**
 * @brief Reads an unsigned 32-bit little-endian integer.
 * @param input Source stream positioned at the encoded integer.
 * @return The decoded integer.
 * @throws std::runtime_error If fewer than four bytes are available.
 */
[[nodiscard]] std::uint32_t read_u32_le(std::istream &input);

/**
 * @brief Reads an unsigned 64-bit little-endian integer.
 * @param input Source stream positioned at the encoded integer.
 * @return The decoded integer.
 * @throws std::runtime_error If fewer than eight bytes are available.
 */
[[nodiscard]] std::uint64_t read_u64_le(std::istream &input);

/**
 * @brief Writes a canonical unsigned LEB128 representation of a 64-bit value.
 * @param output Destination stream positioned at the write location.
 * @param value Integer to encode in at most ten bytes.
 * @throws std::runtime_error If the complete encoding cannot be written.
 */
void write_varint_u64(std::ostream &output, std::uint64_t value);

/**
 * @brief Reads a canonical unsigned LEB128 representation of a 64-bit value.
 * @param input Source stream positioned at the encoded integer.
 * @return The decoded integer.
 * @throws std::runtime_error If the encoding is truncated, overflows 64 bits,
 * exceeds ten bytes, or uses a noncanonical overlong representation.
 */
[[nodiscard]] std::uint64_t read_varint_u64(std::istream &input);

} // namespace snowseek::storage
