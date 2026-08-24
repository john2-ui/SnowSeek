#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace snowseek::storage::detail {

/**
 * @brief Opens an independent binary stream for Segment random access.
 * @param path Existing Segment or spool path.
 * @return An open input stream.
 * @throws std::runtime_error If the path cannot be opened.
 */
[[nodiscard]] inline std::ifstream
open_input(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
                throw std::runtime_error("failed to open index file: " +
                                         path.string());
        }
        return input;
}

/**
 * @brief Positions a stream at an absolute Segment offset.
 * @param input Stream to reposition.
 * @param offset Absolute byte offset from the file start.
 * @throws std::runtime_error If the offset is unsupported or seeking fails.
 */
inline void seek_to(std::istream &input, std::uint64_t offset) {
        if (offset > static_cast<std::uint64_t>(
                             std::numeric_limits<std::streamoff>::max())) {
                throw std::runtime_error("index offset exceeds stream limit");
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset));
        if (!input) {
                throw std::runtime_error("failed to seek index file");
        }
}

/**
 * @brief Reads an exact byte range from the current stream position.
 * @param input Stream supplying bytes.
 * @param data Destination buffer.
 * @param size Required byte count.
 * @throws std::runtime_error If the stream ends early or reports an error.
 */
inline void read_exact(std::istream &input, char *data, std::size_t size) {
        input.read(data, static_cast<std::streamsize>(size));
        if (!input || static_cast<std::size_t>(input.gcount()) != size) {
                throw std::runtime_error("index file is truncated");
        }
}

/**
 * @brief Validates one complete canonical UTF-8 byte sequence.
 * @param bytes Candidate UTF-8 bytes.
 * @return True only for canonical bytes without surrogates or overflow.
 */
[[nodiscard]] inline bool is_valid_utf8(std::string_view bytes) noexcept {
        std::size_t index = 0;
        while (index < bytes.size()) {
                const auto first = static_cast<unsigned char>(bytes[index]);
                std::size_t length = 0;
                if (first <= 0x7fU) {
                        length = 1;
                } else if (first >= 0xc2U && first <= 0xdfU) {
                        length = 2;
                } else if (first >= 0xe0U && first <= 0xefU) {
                        length = 3;
                } else if (first >= 0xf0U && first <= 0xf4U) {
                        length = 4;
                } else {
                        return false;
                }
                if (length > bytes.size() - index) {
                        return false;
                }
                for (std::size_t offset = 1; offset < length; ++offset) {
                        const auto byte = static_cast<unsigned char>(
                                bytes[index + offset]);
                        if (byte < 0x80U || byte > 0xbfU) {
                                return false;
                        }
                        if (offset == 1 &&
                            ((first == 0xe0U && byte < 0xa0U) ||
                             (first == 0xedU && byte > 0x9fU) ||
                             (first == 0xf0U && byte < 0x90U) ||
                             (first == 0xf4U && byte > 0x8fU))) {
                                return false;
                        }
                }
                index += length;
        }
        return true;
}

} // namespace snowseek::storage::detail
