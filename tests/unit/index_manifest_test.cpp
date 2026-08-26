#include "storage/checksum.hpp"
#include "storage/index_manifest.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

/** @brief Stores a little-endian u32 into a serialized Manifest fixture. */
void set_u32(std::string &bytes, std::size_t offset, std::uint32_t value) {
        for (unsigned int byte = 0; byte < 4; ++byte) {
                bytes[offset + byte] =
                        static_cast<char>((value >> (byte * 8U)) & 0xffU);
        }
}

/** @brief Stores a little-endian u64 into a serialized Manifest fixture. */
void set_u64(std::string &bytes, std::size_t offset, std::uint64_t value) {
        for (unsigned int byte = 0; byte < 8; ++byte) {
                bytes[offset + byte] =
                        static_cast<char>((value >> (byte * 8U)) & 0xffU);
        }
}

/** @brief Recomputes the protected Manifest header prefix checksum. */
void refresh_header_checksum(std::string &bytes) {
        set_u32(bytes, 52,
                snowseek::storage::crc32c(
                        std::string_view(bytes).substr(0, 52)));
}

/** @brief Recomputes both payload and header checksums after payload edits. */
void refresh_payload_checksum(std::string &bytes) {
        set_u32(bytes, 48,
                snowseek::storage::crc32c(
                        std::string_view(bytes).substr(
                                snowseek::storage::kManifestHeaderSize)));
        refresh_header_checksum(bytes);
}

/** @brief Requires strict decoding to reject one mutated byte sequence. */
void require_rejected(std::string_view bytes, std::string_view message) {
        snowseek::test::require_throws<std::runtime_error>(
                [bytes] {
                        static_cast<void>(
                                snowseek::storage::decode_manifest(bytes));
                },
                message);
}

/** @brief Verifies deterministic layout, filename derivation, and round-trip. */
void round_trips_deterministically() {
        const snowseek::storage::IndexManifest expected{7, 43, {42}};
        const auto first = snowseek::storage::encode_manifest(expected);
        const auto second = snowseek::storage::encode_manifest(expected);
        const auto actual = snowseek::storage::decode_manifest(first);

        snowseek::test::require_equal(first, second,
                                      "Manifest bytes should be deterministic");
        snowseek::test::require_equal(
                first.size(), std::size_t{72},
                "one-Segment Manifest should contain 64+8 bytes");
        snowseek::test::require_equal(actual.generation, std::uint64_t{7},
                                      "generation should round-trip");
        snowseek::test::require_equal(actual.next_segment_id,
                                      snowseek::storage::SegmentId{43},
                                      "next SegmentId should round-trip");
        snowseek::test::require_equal(actual.active_segments,
                                      std::vector<snowseek::storage::SegmentId>{42},
                                      "active SegmentId should round-trip");
        snowseek::test::require_equal(
                snowseek::storage::segment_file_name(42),
                std::string("segment-0000000000000042.idx"),
                "Segment filename should use sixteen decimal digits");

        const snowseek::storage::IndexManifest multiple{8, 45, {1, 17, 44}};
        const auto multiple_bytes =
                snowseek::storage::encode_manifest(multiple);
        const auto multiple_actual =
                snowseek::storage::decode_manifest(multiple_bytes);
        snowseek::test::require_equal(
                multiple_bytes.size(), std::size_t{88},
                "three-Segment Manifest should contain 64+3*8 bytes");
        snowseek::test::require_equal(
                multiple_actual.active_segments, multiple.active_segments,
                "ordered active SegmentIds should round-trip");
}

/** @brief Verifies identity, sizing, truncation, and reserved fields. */
void rejects_invalid_structure() {
        const auto valid = snowseek::storage::encode_manifest({1, 2, {1}});
        require_rejected(std::string_view(valid).substr(0, 63),
                         "truncated header should fail");
        require_rejected(std::string_view(valid).substr(0, 71),
                         "truncated payload should fail");

        auto bytes = valid;
        bytes[0] = 'X';
        require_rejected(bytes, "invalid magic should fail");
        bytes = valid;
        set_u32(bytes, 8, 2);
        refresh_header_checksum(bytes);
        require_rejected(bytes, "unsupported version should fail");
        bytes = valid;
        set_u32(bytes, 12, 1);
        refresh_header_checksum(bytes);
        require_rejected(bytes, "nonzero flags should fail");
        bytes = valid;
        set_u32(bytes, 16, 63);
        refresh_header_checksum(bytes);
        require_rejected(bytes, "unknown header size should fail");
        bytes = valid;
        set_u32(bytes, 20, 0);
        refresh_header_checksum(bytes);
        require_rejected(bytes, "an empty active Segment list should fail");
        bytes = valid;
        set_u64(bytes, 40, 16);
        refresh_header_checksum(bytes);
        require_rejected(bytes, "inconsistent payload length should fail");
        bytes = valid;
        set_u64(bytes, 56, 1);
        require_rejected(bytes, "nonzero reserved bytes should fail");
}

/** @brief Verifies both independent CRCs reject protected corruption. */
void rejects_checksum_corruption() {
        const auto valid = snowseek::storage::encode_manifest({1, 2, {1}});
        auto bytes = valid;
        bytes[24] ^= 1;
        require_rejected(bytes, "header corruption should fail its CRC");
        bytes = valid;
        bytes[64] ^= 1;
        require_rejected(bytes, "payload corruption should fail its CRC");
}

/** @brief Verifies semantic counters and identifiers after valid CRC repair. */
void rejects_invalid_semantics() {
        const auto valid = snowseek::storage::encode_manifest({1, 2, {1}});
        auto bytes = valid;
        set_u64(bytes, 24, 0);
        refresh_header_checksum(bytes);
        require_rejected(bytes, "zero generation should fail");
        bytes = valid;
        set_u64(bytes, 32, 1);
        refresh_header_checksum(bytes);
        require_rejected(bytes, "next ID must exceed active ID");
        bytes = valid;
        set_u64(bytes, 64, 0);
        refresh_payload_checksum(bytes);
        require_rejected(bytes, "zero active ID should fail");

        snowseek::test::require_throws<std::runtime_error>(
                [] {
                        static_cast<void>(snowseek::storage::encode_manifest(
                                {1, 4, {2, 1}}));
                },
                "encoder should reject decreasing active SegmentIds");
        snowseek::test::require_throws<std::runtime_error>(
                [] {
                        static_cast<void>(snowseek::storage::encode_manifest(
                                {1, 4, {1, 1}}));
                },
                "encoder should reject duplicate active SegmentIds");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(
                                snowseek::storage::segment_file_name(0));
                },
                "zero SegmentId should not have a filename");
}

} // namespace

/** @brief Runs Manifest v1 codec and invariant tests. */
int main() {
        return snowseek::test::run({
                {"round trips deterministically", round_trips_deterministically},
                {"rejects invalid structure", rejects_invalid_structure},
                {"rejects checksum corruption", rejects_checksum_corruption},
                {"rejects invalid semantics", rejects_invalid_semantics},
        });
}
