/**
 * @file binary_codec.cpp
 * @brief Implements fixed-width little-endian stream encoding helpers.
 */

#include "storage/binary_codec.hpp"

#include <array>
#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace snowseek::storage {
namespace {

/**
 * @brief Reads one complete fixed-width encoded value.
 * @tparam Size Encoded width in bytes.
 * @param input Source stream positioned at the value.
 * @param bytes Destination byte buffer.
 * @param value_name Name included in the truncation diagnostic.
 * @throws std::runtime_error If the complete value is unavailable.
 */
template <std::size_t Size>
void read_bytes(std::istream &input, std::array<unsigned char, Size> &bytes,
                std::string_view value_name) {
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size()) {
                throw std::runtime_error("truncated " +
                                         std::string(value_name));
        }
}

/**
 * @brief Ensures a completed fixed-width write reached the stream.
 * @param output Destination stream to inspect.
 * @throws std::runtime_error If the stream rejected any output byte.
 */
void require_write(std::ostream &output) {
        if (!output) {
                throw std::runtime_error("failed to write binary value");
        }
}

} // namespace

void write_u32_le(std::ostream &output, std::uint32_t value) {
        const std::array<char, 4> bytes{
                static_cast<char>(value & 0xffU),
                static_cast<char>((value >> 8U) & 0xffU),
                static_cast<char>((value >> 16U) & 0xffU),
                static_cast<char>((value >> 24U) & 0xffU),
        };
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require_write(output);
}

void write_u64_le(std::ostream &output, std::uint64_t value) {
        const std::array<char, 8> bytes{
                static_cast<char>(value & 0xffU),
                static_cast<char>((value >> 8U) & 0xffU),
                static_cast<char>((value >> 16U) & 0xffU),
                static_cast<char>((value >> 24U) & 0xffU),
                static_cast<char>((value >> 32U) & 0xffU),
                static_cast<char>((value >> 40U) & 0xffU),
                static_cast<char>((value >> 48U) & 0xffU),
                static_cast<char>((value >> 56U) & 0xffU),
        };
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require_write(output);
}

std::uint32_t read_u32_le(std::istream &input) {
        std::array<unsigned char, 4> bytes{};
        read_bytes(input, bytes, "u32");
        return static_cast<std::uint32_t>(bytes[0]) |
               (static_cast<std::uint32_t>(bytes[1]) << 8U) |
               (static_cast<std::uint32_t>(bytes[2]) << 16U) |
               (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t read_u64_le(std::istream &input) {
        std::array<unsigned char, 8> bytes{};
        read_bytes(input, bytes, "u64");
        return static_cast<std::uint64_t>(bytes[0]) |
               (static_cast<std::uint64_t>(bytes[1]) << 8U) |
               (static_cast<std::uint64_t>(bytes[2]) << 16U) |
               (static_cast<std::uint64_t>(bytes[3]) << 24U) |
               (static_cast<std::uint64_t>(bytes[4]) << 32U) |
               (static_cast<std::uint64_t>(bytes[5]) << 40U) |
               (static_cast<std::uint64_t>(bytes[6]) << 48U) |
               (static_cast<std::uint64_t>(bytes[7]) << 56U);
}

} // namespace snowseek::storage
