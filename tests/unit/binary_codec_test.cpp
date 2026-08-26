/**
 * @file binary_codec_test.cpp
 * @brief Verifies little-endian binary encoding, decoding, and failure
 * handling.
 */

#include "storage/binary_codec.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

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
}

} // namespace

/** @brief Runs the binary-codec unit-test suite. */
int main() {
        return snowseek::test::run({
                {"round-trips fixed-width integers",
                 round_trips_fixed_width_integers},
                {"rejects truncated fixed-width integers",
                 rejects_truncated_fixed_width_integers},
                {"rejects failed output streams",
                 rejects_failed_output_streams},
        });
}
