#include "snowseek/document/text_reader.hpp"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace snowseek::document {
namespace {

constexpr std::string_view kReplacementCharacter{"\xef\xbf\xbd", 3};

[[nodiscard]] bool is_continuation(unsigned char byte) {
        return byte >= 0x80U && byte <= 0xbfU;
}

[[nodiscard]] std::size_t sequence_length(unsigned char leading_byte) {
        if (leading_byte <= 0x7fU) {
                return 1;
        }
        if (leading_byte >= 0xc2U && leading_byte <= 0xdfU) {
                return 2;
        }
        if (leading_byte >= 0xe0U && leading_byte <= 0xefU) {
                return 3;
        }
        if (leading_byte >= 0xf0U && leading_byte <= 0xf4U) {
                return 4;
        }
        return 0;
}

[[nodiscard]] bool is_valid_second_byte(unsigned char leading_byte,
                                        unsigned char second_byte) {
        if (!is_continuation(second_byte)) {
                return false;
        }
        if (leading_byte == 0xe0U) {
                return second_byte >= 0xa0U;
        }
        if (leading_byte == 0xedU) {
                return second_byte <= 0x9fU;
        }
        if (leading_byte == 0xf0U) {
                return second_byte >= 0x90U;
        }
        if (leading_byte == 0xf4U) {
                return second_byte <= 0x8fU;
        }
        return true;
}

void handle_invalid_sequence(const std::filesystem::path &path,
                             InvalidUtf8Policy policy,
                             std::uint64_t byte_offset, TextReadStats &stats,
                             std::string &output) {
        if (policy == InvalidUtf8Policy::reject) {
                throw InvalidUtf8Error(path, byte_offset);
        }
        output.append(kReplacementCharacter);
        ++stats.invalid_sequence_count;
}

struct Utf8State {
        std::string pending;
        std::uint64_t pending_offset{};
};

void emit_valid_utf8(const std::filesystem::path &path,
                     InvalidUtf8Policy policy, std::string_view bytes,
                     std::uint64_t base_offset, bool final,
                     TextReadStats &stats, Utf8State &state,
                     const TextChunkConsumer &consumer) {
        std::string input;
        if (!state.pending.empty()) {
                input.reserve(state.pending.size() + bytes.size());
                input.append(state.pending);
                input.append(bytes);
                base_offset = state.pending_offset;
                state.pending.clear();
        } else {
                input.assign(bytes);
        }

        std::string output;
        output.reserve(input.size());

        std::size_t index = 0;
        while (index < input.size()) {
                const auto leading_byte =
                        static_cast<unsigned char>(input[index]);
                const std::size_t expected_length =
                        sequence_length(leading_byte);

                if (expected_length == 1) {
                        output.push_back(input[index]);
                        ++index;
                        continue;
                }

                if (expected_length == 0) {
                        handle_invalid_sequence(path, policy,
                                                base_offset + index, stats,
                                                output);
                        ++index;
                        continue;
                }

                const std::size_t available = input.size() - index;
                const std::size_t bytes_to_check = available < expected_length
                                                           ? available
                                                           : expected_length;
                std::size_t valid_prefix = 1;

                for (std::size_t offset = 1; offset < bytes_to_check;
                     ++offset) {
                        const auto byte = static_cast<unsigned char>(
                                input[index + offset]);
                        const bool valid =
                                offset == 1 ? is_valid_second_byte(leading_byte,
                                                                   byte)
                                            : is_continuation(byte);
                        if (!valid) {
                                break;
                        }
                        ++valid_prefix;
                }

                if (valid_prefix < bytes_to_check) {
                        handle_invalid_sequence(path, policy,
                                                base_offset + index, stats,
                                                output);
                        index += valid_prefix;
                        continue;
                }

                if (available < expected_length) {
                        if (!final) {
                                state.pending.assign(input, index,
                                                     std::string::npos);
                                state.pending_offset = base_offset + index;
                                break;
                        }
                        handle_invalid_sequence(path, policy,
                                                base_offset + index, stats,
                                                output);
                        index = input.size();
                        continue;
                }

                output.append(input, index, expected_length);
                index += expected_length;
        }

        if (!output.empty()) {
                stats.bytes_emitted += output.size();
                consumer(output);
        }
}

[[nodiscard]] std::runtime_error read_error(const std::filesystem::path &path,
                                            std::string_view action) {
        return std::runtime_error("failed to " + std::string(action) +
                                  " text file: " + path.string());
}

} // namespace

InvalidUtf8Error::InvalidUtf8Error(std::filesystem::path path,
                                   std::uint64_t byte_offset)
    : std::runtime_error("invalid UTF-8 at byte offset " +
                         std::to_string(byte_offset) + ": " + path.string()),
      path_(std::move(path)), byte_offset_(byte_offset) {}

const std::filesystem::path &InvalidUtf8Error::path() const noexcept {
        return path_;
}

std::uint64_t InvalidUtf8Error::byte_offset() const noexcept {
        return byte_offset_;
}

TextReader::TextReader(TextReadOptions options) : options_(options) {
        const auto maximum_stream_size = static_cast<std::uintmax_t>(
                std::numeric_limits<std::streamsize>::max());
        if (options_.chunk_size == 0 ||
            options_.chunk_size > maximum_stream_size) {
                throw std::invalid_argument(
                        "text reader chunk size is outside the valid range");
        }
}

TextReadStats TextReader::read(const std::filesystem::path &path,
                               const TextChunkConsumer &consumer) const {
        if (!consumer) {
                throw std::invalid_argument(
                        "text reader consumer must not be empty");
        }

        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
                throw read_error(path, "open");
        }

        TextReadStats stats;
        Utf8State utf8_state;
        std::vector<char> buffer(options_.chunk_size);

        while (true) {
                input.read(buffer.data(),
                           static_cast<std::streamsize>(buffer.size()));
                const std::streamsize count = input.gcount();

                if (count > 0) {
                        const auto byte_count = static_cast<std::size_t>(count);
                        const std::uint64_t chunk_offset = stats.bytes_read;
                        stats.bytes_read += byte_count;
                        emit_valid_utf8(
                                path, options_.invalid_utf8_policy,
                                std::string_view(buffer.data(), byte_count),
                                chunk_offset, false, stats, utf8_state,
                                consumer);
                }

                if (input.bad() || (input.fail() && !input.eof())) {
                        throw read_error(path, "read");
                }
                if (input.eof()) {
                        break;
                }
        }

        emit_valid_utf8(path, options_.invalid_utf8_policy, {},
                        stats.bytes_read, true, stats, utf8_state, consumer);
        return stats;
}

} // namespace snowseek::document
