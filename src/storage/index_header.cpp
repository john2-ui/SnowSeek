#include "snowseek/storage/index_header.hpp"

#include <istream>
#include <ostream>
#include <stdexcept>

namespace snowseek::storage {
namespace {

void write_u32_le(std::ostream &output, std::uint32_t value) {
        for (unsigned int shift = 0; shift < 32; shift += 8) {
                output.put(static_cast<char>((value >> shift) & 0xffU));
        }
}

std::uint32_t read_u32_le(std::istream &input) {
        std::uint32_t value = 0;
        for (unsigned int shift = 0; shift < 32; shift += 8) {
                const int byte = input.get();
                if (byte == std::char_traits<char>::eof()) {
                        throw std::runtime_error("truncated index header");
                }
                value |= static_cast<std::uint32_t>(
                                 static_cast<unsigned char>(byte))
                         << shift;
        }
        return value;
}

} // namespace

void write_header(std::ostream &output, const IndexHeader &header) {
        output.write(kIndexMagic.data(),
                     static_cast<std::streamsize>(kIndexMagic.size()));
        write_u32_le(output, header.version);
        write_u32_le(output, header.feature_flags);
        if (!output) {
                throw std::runtime_error("failed to write index header");
        }
}

IndexHeader read_header(std::istream &input) {
        std::array<char, kIndexMagic.size()> magic{};
        input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!input || magic != kIndexMagic) {
                throw std::runtime_error("invalid SnowSeek index magic");
        }
        return IndexHeader{read_u32_le(input), read_u32_le(input)};
}

} // namespace snowseek::storage
