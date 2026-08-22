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
 * @brief Ensures a completed fixed-width or Varint write reached the stream.
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

void write_varint_u64(std::ostream &output, std::uint64_t value) {
        do {
                std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fU);
                value >>= 7U;
                if (value != 0) {
                        byte |= 0x80U;
                }
                output.put(static_cast<char>(byte));
        } while (value != 0);
        require_write(output);
}

std::uint64_t read_varint_u64(std::istream &input) {
        std::uint64_t value = 0;
        for (unsigned int byte_index = 0; byte_index < 10; ++byte_index) {
                const auto byte = read_byte(input, "Varint");
                const auto payload = static_cast<std::uint8_t>(byte & 0x7fU);

                if (byte_index == 9 && (byte & 0xfeU) != 0) {
                        throw std::runtime_error("Varint exceeds uint64_t");
                }
                value |= static_cast<std::uint64_t>(payload)
                         << (byte_index * 7U);

                if ((byte & 0x80U) == 0) {
                        if (byte_index != 0 && payload == 0) {
                                throw std::runtime_error(
                                        "Varint encoding is not canonical");
                        }
                        return value;
                }
        }
        throw std::runtime_error("Varint exceeds ten bytes");
}

} // namespace snowseek::storage
