#include "snowseek/storage/binary_codec.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/** @brief Verifies exact little-endian byte order and boundary round trips. */
void round_trips_fixed_width_integers() {
        std::stringstream stream(std::ios::in | std::ios::out |
                                 std::ios::binary);
        snowseek::storage::write_u32_le(stream, 0);
        snowseek::storage::write_u32_le(stream, 0x78563412U);
        snowseek::storage::write_u64_le(
                stream, std::numeric_limits<std::uint64_t>::max());

        const std::string expected{
                '\0',   '\0',   '\0',   '\0',   '\x12', '\x34', '\x56', '\x78',
                '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff'};
        snowseek::test::require_equal(
                stream.str(), expected,
                "fixed-width integers should use exact little-endian bytes");

        stream.seekg(0);
        snowseek::test::require_equal(snowseek::storage::read_u32_le(stream),
                                      std::uint32_t{0},
                                      "zero u32 should round-trip");
        snowseek::test::require_equal(snowseek::storage::read_u32_le(stream),
                                      std::uint32_t{0x78563412U},
                                      "nonzero u32 should round-trip");
        snowseek::test::require_equal(snowseek::storage::read_u64_le(stream),
                                      std::numeric_limits<std::uint64_t>::max(),
                                      "maximum u64 should round-trip");
}

/** @brief Verifies rejection of truncated fixed-width integers. */
void rejects_truncated_fixed_width_integers() {
        std::istringstream short_u32(std::string(3, '\0'),
                                     std::ios::in | std::ios::binary);
        snowseek::test::require_throws<std::runtime_error>(
                [&short_u32] {
                        static_cast<void>(
                                snowseek::storage::read_u32_le(short_u32));
                },
                "a three-byte u32 should be rejected");

        std::istringstream short_u64(std::string(7, '\0'),
                                     std::ios::in | std::ios::binary);
        snowseek::test::require_throws<std::runtime_error>(
                [&short_u64] {
                        static_cast<void>(
                                snowseek::storage::read_u64_le(short_u64));
                },
                "a seven-byte u64 should be rejected");
}

/** @brief Verifies canonical Varint round trips at encoding boundaries. */
void round_trips_varint_boundaries() {
        const std::vector<std::uint64_t> values{
                0,
                127,
                128,
                16'383,
                16'384,
                std::numeric_limits<std::uint32_t>::max(),
                std::numeric_limits<std::uint64_t>::max(),
        };

        for (const auto expected : values) {
                std::stringstream stream(std::ios::in | std::ios::out |
                                         std::ios::binary);
                snowseek::storage::write_varint_u64(stream, expected);
                stream.seekg(0);
                snowseek::test::require_equal(
                        snowseek::storage::read_varint_u64(stream), expected,
                        "Varint boundary value should round-trip");
        }

        std::ostringstream encoded(std::ios::out | std::ios::binary);
        snowseek::storage::write_varint_u64(encoded, 128);
        snowseek::test::require_equal(
                encoded.str(), std::string{'\x80', '\x01'},
                "128 should use its canonical two-byte encoding");
}

/** @brief Verifies malformed and noncanonical Varints are rejected. */
void rejects_invalid_varints() {
        std::istringstream truncated(std::string{'\x80'},
                                     std::ios::in | std::ios::binary);
        snowseek::test::require_throws<std::runtime_error>(
                [&truncated] {
                        static_cast<void>(
                                snowseek::storage::read_varint_u64(truncated));
                },
                "a truncated Varint should be rejected");

        std::istringstream noncanonical(std::string{'\x81', '\x00'},
                                        std::ios::in | std::ios::binary);
        snowseek::test::require_throws<std::runtime_error>(
                [&noncanonical] {
                        static_cast<void>(snowseek::storage::read_varint_u64(
                                noncanonical));
                },
                "an overlong Varint should be rejected");

        std::string tenth_byte_overflow(9, static_cast<char>(0x80));
        tenth_byte_overflow.push_back('\x02');
        std::istringstream overflow(tenth_byte_overflow,
                                    std::ios::in | std::ios::binary);
        snowseek::test::require_throws<std::runtime_error>(
                [&overflow] {
                        static_cast<void>(
                                snowseek::storage::read_varint_u64(overflow));
                },
                "a Varint using high bits in byte ten should be rejected");

        std::string too_long(11, static_cast<char>(0x80));
        std::istringstream excessive(too_long, std::ios::in | std::ios::binary);
        snowseek::test::require_throws<std::runtime_error>(
                [&excessive] {
                        static_cast<void>(
                                snowseek::storage::read_varint_u64(excessive));
                },
                "a Varint longer than ten bytes should be rejected");
}

/** @brief Verifies every encoder reports an already-failed output stream. */
void rejects_failed_output_streams() {
        std::ostringstream fixed_width;
        fixed_width.setstate(std::ios::badbit);
        snowseek::test::require_throws<std::runtime_error>(
                [&fixed_width] {
                        snowseek::storage::write_u32_le(fixed_width, 1);
                },
                "u32 writes should report stream failure");

        std::ostringstream wide_fixed_width;
        wide_fixed_width.setstate(std::ios::badbit);
        snowseek::test::require_throws<std::runtime_error>(
                [&wide_fixed_width] {
                        snowseek::storage::write_u64_le(wide_fixed_width, 1);
                },
                "u64 writes should report stream failure");

        std::ostringstream varint;
        varint.setstate(std::ios::badbit);
        snowseek::test::require_throws<std::runtime_error>(
                [&varint] { snowseek::storage::write_varint_u64(varint, 1); },
                "Varint writes should report stream failure");
}

} // namespace

/** @brief Runs the binary-codec unit-test suite. */
int main() {
        return snowseek::test::run({
                {"round-trips fixed-width integers",
                 round_trips_fixed_width_integers},
                {"rejects truncated fixed-width integers",
                 rejects_truncated_fixed_width_integers},
                {"round-trips Varint boundaries",
                 round_trips_varint_boundaries},
                {"rejects invalid Varints", rejects_invalid_varints},
                {"rejects failed output streams",
                 rejects_failed_output_streams},
        });
}
