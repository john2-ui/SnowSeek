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

/**
 * @brief Tests whether a byte is a UTF-8 continuation byte.
 * @param byte Byte to classify.
 * @return True when byte is in the 0x80 through 0xBF range.
 */
[[nodiscard]] bool is_continuation(unsigned char byte) {
        return byte >= 0x80U && byte <= 0xbfU;
}

/**
 * @brief Determines the expected UTF-8 sequence length from a leading byte.
 * @param leading_byte Candidate leading byte.
 * @return A length from one through four, or zero for an invalid leader.
 */
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

/**
 * @brief Validates the constrained second byte of a UTF-8 sequence.
 * @param leading_byte Valid multibyte sequence leader.
 * @param second_byte Candidate second byte.
 * @return True when the pair cannot encode an overlong, surrogate, or
 * out-of-range code point.
 */
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

/**
 * @brief Applies the configured policy to one invalid UTF-8 sequence.
 * @param path Source file being decoded.
 * @param policy Whether to replace or reject invalid input.
 * @param byte_offset Original source offset of the invalid sequence.
 * @param stats Statistics updated when replacement is selected.
 * @param output Current output buffer receiving the replacement character.
 * @throws InvalidUtf8Error If policy requests strict rejection.
 */
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

/**
 * @brief Validates one source chunk and emits complete UTF-8 sequences.
 * @param path Source file used in diagnostics.
 * @param policy Invalid-sequence handling policy.
 * @param bytes Newly read source bytes.
 * @param base_offset Source offset corresponding to bytes.front().
 * @param final Whether no additional source bytes will arrive.
 * @param stats Read statistics updated with emitted and invalid byte counts.
 * @param state Cross-chunk state holding an incomplete final sequence.
 * @param consumer Callback receiving validated nonempty output chunks.
 * @throws InvalidUtf8Error If strict mode encounters invalid UTF-8.
 */
void emit_valid_utf8(const std::filesystem::path &path,
                     InvalidUtf8Policy policy, std::string_view bytes,
                     std::uint64_t base_offset, bool final,
                     TextReadStats &stats, Utf8State &state,
                     const TextChunkConsumer &consumer) {
        std::string input;

        // Reattach a sequence split by the previous read before validating the
        // new bytes against their original file offsets.
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
                                // Preserve only a valid incomplete suffix; it
                                // will be completed by the next reader chunk.
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

        // Consumer views borrow output storage and therefore cannot escape this
        // synchronous callback.
        if (!output.empty()) {
                stats.bytes_emitted += output.size();
                consumer(output);
        }
}

/**
 * @brief Creates a consistent file-operation failure diagnostic.
 * @param path File involved in the failed operation.
 * @param action Operation name such as open or read.
 * @return A runtime error containing the action and path.
 */
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
