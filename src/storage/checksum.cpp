/**
 * @file checksum.cpp
 * @brief Implements portable table-driven CRC32C checksums.
 */

#include "storage/checksum.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace snowseek::storage {
namespace {

constexpr std::uint32_t kReflectedCastagnoliPolynomial = 0x82f63b78U;

/**
 * @brief Builds slicing-by-eight tables for reflected CRC32C updates.
 * @return Tables ordered from one-byte through eight-byte advancement.
 */
constexpr std::array<std::array<std::uint32_t, 256>, 8>
make_crc32c_tables() {
        std::array<std::array<std::uint32_t, 256>, 8> tables{};
        for (std::uint32_t index = 0; index < tables.front().size(); ++index) {
                std::uint32_t entry = index;
                for (unsigned int bit = 0; bit < 8; ++bit) {
                        entry = (entry & 1U) != 0
                                        ? (entry >> 1U) ^
                                                  kReflectedCastagnoliPolynomial
                                        : entry >> 1U;
                }
                tables[0][index] = entry;
        }
        for (std::size_t slice = 1; slice < tables.size(); ++slice) {
                for (std::size_t index = 0; index < tables[slice].size();
                     ++index) {
                        const auto previous = tables[slice - 1][index];
                        tables[slice][index] =
                                tables[0][previous & 0xffU] ^ (previous >> 8U);
                }
        }
        return tables;
}

constexpr auto kCrc32cTables = make_crc32c_tables();

} // namespace

void Crc32c::update(std::string_view bytes) noexcept {
        const auto *current = reinterpret_cast<const unsigned char *>(
                bytes.data());
        auto remaining = bytes.size();
        while (remaining >= 8) {
                const auto first = state_ ^
                                   static_cast<std::uint32_t>(current[0]) ^
                                   (static_cast<std::uint32_t>(current[1])
                                    << 8U) ^
                                   (static_cast<std::uint32_t>(current[2])
                                    << 16U) ^
                                   (static_cast<std::uint32_t>(current[3])
                                    << 24U);
                const auto second =
                        static_cast<std::uint32_t>(current[4]) ^
                        (static_cast<std::uint32_t>(current[5]) << 8U) ^
                        (static_cast<std::uint32_t>(current[6]) << 16U) ^
                        (static_cast<std::uint32_t>(current[7]) << 24U);
                state_ = kCrc32cTables[7][first & 0xffU] ^
                         kCrc32cTables[6][(first >> 8U) & 0xffU] ^
                         kCrc32cTables[5][(first >> 16U) & 0xffU] ^
                         kCrc32cTables[4][first >> 24U] ^
                         kCrc32cTables[3][second & 0xffU] ^
                         kCrc32cTables[2][(second >> 8U) & 0xffU] ^
                         kCrc32cTables[1][(second >> 16U) & 0xffU] ^
                         kCrc32cTables[0][second >> 24U];
                current += 8;
                remaining -= 8;
        }
        while (remaining != 0) {
                const auto byte = *current++;
                const auto table_index = static_cast<std::uint8_t>(
                        state_ ^ static_cast<std::uint8_t>(byte));
                state_ = kCrc32cTables[0][table_index] ^ (state_ >> 8U);
                --remaining;
        }
}

std::uint32_t Crc32c::value() const noexcept {
        return state_ ^ 0xffffffffU;
}

std::uint32_t crc32c(std::string_view bytes) noexcept {
        Crc32c checksum;
        checksum.update(bytes);
        return checksum.value();
}

} // namespace snowseek::storage
