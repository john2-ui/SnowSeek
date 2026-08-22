#include "snowseek/storage/binary_codec.hpp"

#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace snowseek::storage {
namespace {

/**
 * @brief Reads one byte or reports a truncated encoded value.
 * @param input Source stream positioned at the byte.
 * @param value_name Name included in the truncation diagnostic.
 * @return The byte converted to an unsigned integer.
 * @throws std::runtime_error If no byte is available.
 */
[[nodiscard]] std::uint8_t read_byte(std::istream &input,
                                     std::string_view value_name) {
        const int byte = input.get();
        if (byte == std::char_traits<char>::eof()) {
                throw std::runtime_error("truncated " +
                                         std::string(value_name));
        }
        return static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
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
        for (unsigned int shift = 0; shift < 32; shift += 8) {
                output.put(static_cast<char>((value >> shift) & 0xffU));
        }
        require_write(output);
}

void write_u64_le(std::ostream &output, std::uint64_t value) {
        for (unsigned int shift = 0; shift < 64; shift += 8) {
                output.put(static_cast<char>((value >> shift) & 0xffU));
        }
        require_write(output);
}

std::uint32_t read_u32_le(std::istream &input) {
        std::uint32_t value = 0;
        for (unsigned int shift = 0; shift < 32; shift += 8) {
                value |= static_cast<std::uint32_t>(read_byte(input, "u32"))
                         << shift;
        }
        return value;
}

std::uint64_t read_u64_le(std::istream &input) {
        std::uint64_t value = 0;
        for (unsigned int shift = 0; shift < 64; shift += 8) {
                value |= static_cast<std::uint64_t>(read_byte(input, "u64"))
                         << shift;
        }
        return value;
}

} // namespace snowseek::storage
