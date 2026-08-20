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

void write_header(std::ostream &output, const IndexHeader &header);
[[nodiscard]] IndexHeader read_header(std::istream &input);

} // namespace snowseek::storage
