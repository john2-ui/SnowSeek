#include "snowseek/storage/checksum.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace snowseek::storage {
namespace {

constexpr std::uint32_t kReflectedCastagnoliPolynomial = 0x82f63b78U;

/**
 * @brief Builds the portable lookup table for reflected CRC32C updates.
 * @return Table indexed by the low byte of the current CRC state.
 */
constexpr std::array<std::uint32_t, 256> make_crc32c_table() {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t index = 0; index < table.size(); ++index) {
                std::uint32_t entry = index;
                for (unsigned int bit = 0; bit < 8; ++bit) {
                        entry = (entry & 1U) != 0
                                        ? (entry >> 1U) ^
                                                  kReflectedCastagnoliPolynomial
                                        : entry >> 1U;
                }
                table[index] = entry;
        }
        return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();

} // namespace

std::uint32_t crc32c(std::string_view bytes) noexcept {
        std::uint32_t state = 0xffffffffU;
        for (const unsigned char byte : bytes) {
                const auto table_index = static_cast<std::uint8_t>(
                        state ^ static_cast<std::uint8_t>(byte));
                state = kCrc32cTable[table_index] ^ (state >> 8U);
        }
        return state ^ 0xffffffffU;
}

} // namespace snowseek::storage
