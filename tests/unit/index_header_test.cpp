#include "snowseek/storage/index_header.hpp"

#include "snowseek/storage/binary_codec.hpp"
#include "snowseek/storage/checksum.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

/**
 * @brief Creates a valid packed header with nonempty sections.
 * @return Header suitable for round-trip and corruption tests.
 */
[[nodiscard]] snowseek::storage::IndexHeader make_header() {
        snowseek::storage::IndexHeader header;
        const std::uint64_t lengths[]{8, 4, 72, 32, 16};
        std::uint64_t offset = snowseek::storage::kIndexHeaderSize;
        for (std::size_t index = 0; index < header.sections.size(); ++index) {
                auto &section = header.sections[index];
                section.offset = offset;
                section.length = lengths[index];
                section.checksum = static_cast<std::uint32_t>(index + 1);
                offset += section.length;
        }
        header.file_size = offset;
        return header;
}

/**
 * @brief Serializes a valid header into its complete fixed-width form.
 * @param header Header to encode.
 * @return Exactly 200 serialized bytes.
 * @throws std::runtime_error If header validation or writing fails.
 */
[[nodiscard]] std::string
serialize(const snowseek::storage::IndexHeader &header) {
        std::ostringstream stream(std::ios::out | std::ios::binary);
        snowseek::storage::write_header(stream, header);
        return stream.str();
}

/**
 * @brief Replaces one little-endian u32 in a serialized test header.
 * @param bytes Mutable complete header bytes.
 * @param offset Byte offset of the field to replace.
 * @param value New integer value.
 */
void set_u32(std::string &bytes, std::size_t offset, std::uint32_t value) {
        for (unsigned int byte = 0; byte < 4; ++byte) {
                bytes[offset + byte] =
                        static_cast<char>((value >> (byte * 8U)) & 0xffU);
        }
}

/**
 * @brief Replaces one little-endian u64 in a serialized test header.
 * @param bytes Mutable complete header bytes.
 * @param offset Byte offset of the field to replace.
 * @param value New integer value.
 */
void set_u64(std::string &bytes, std::size_t offset, std::uint64_t value) {
        for (unsigned int byte = 0; byte < 8; ++byte) {
                bytes[offset + byte] =
                        static_cast<char>((value >> (byte * 8U)) & 0xffU);
        }
}

/**
 * @brief Recomputes the CRC after intentionally mutating protected bytes.
 * @param bytes Mutable complete header bytes.
 */
void refresh_checksum(std::string &bytes) {
        const auto checksum = snowseek::storage::crc32c(
                std::string_view(bytes).substr(0, 192));
        set_u32(bytes, 192, checksum);
}

/**
 * @brief Requires a serialized header to fail strict decoding.
 * @param bytes Header fixture to decode.
 * @param message Assertion diagnostic used when decoding succeeds.
 */
void require_rejected(const std::string &bytes, std::string_view message) {
        std::istringstream stream(bytes, std::ios::in | std::ios::binary);
        snowseek::test::require_throws<std::runtime_error>(
                [&stream] {
                        static_cast<void>(
                                snowseek::storage::read_header(stream));
                },
                message);
}

/** @brief Verifies every logical header field survives serialization. */
void round_trips_header() {
        const auto expected = make_header();
        const auto bytes = serialize(expected);
        std::istringstream stream(bytes, std::ios::in | std::ios::binary);
        const auto actual = snowseek::storage::read_header(stream);

        snowseek::test::require_equal(actual.version, expected.version,
                                      "index version should round-trip");
        snowseek::test::require_equal(actual.feature_flags,
                                      expected.feature_flags,
                                      "index feature flags should round-trip");
        snowseek::test::require_equal(actual.file_size, expected.file_size,
                                      "index file size should round-trip");
        for (std::size_t index = 0; index < actual.sections.size(); ++index) {
                snowseek::test::require_equal(actual.sections[index].kind,
                                              expected.sections[index].kind,
                                              "section kind should round-trip");
                snowseek::test::require_equal(
                        actual.sections[index].offset,
                        expected.sections[index].offset,
                        "section offset should round-trip");
                snowseek::test::require_equal(
                        actual.sections[index].length,
                        expected.sections[index].length,
                        "section length should round-trip");
                snowseek::test::require_equal(
                        actual.sections[index].checksum,
                        expected.sections[index].checksum,
                        "section checksum should round-trip");
        }
}

/** @brief Verifies the stable 200-byte layout and Header CRC coverage. */
void writes_stable_header_layout() {
        const auto bytes = serialize(make_header());
        snowseek::test::require_equal(
                bytes.size(),
                static_cast<std::size_t>(snowseek::storage::kIndexHeaderSize),
                "serialized header should contain exactly 200 bytes");
        snowseek::test::require_equal(
                bytes.substr(0, snowseek::storage::kIndexMagic.size()),
                std::string(snowseek::storage::kIndexMagic.begin(),
                            snowseek::storage::kIndexMagic.end()),
                "serialized header should begin with SnowSeek magic");
        snowseek::test::require(
                static_cast<unsigned char>(bytes[8]) == 1U &&
                        static_cast<unsigned char>(bytes[16]) == 200U &&
                        static_cast<unsigned char>(bytes[20]) == 5U,
                "fixed header fields should occupy their documented offsets");

        std::istringstream checksum_stream(bytes.substr(192, 4),
                                           std::ios::in | std::ios::binary);
        const auto stored_checksum =
                snowseek::storage::read_u32_le(checksum_stream);
        snowseek::test::require_equal(
                stored_checksum,
                snowseek::storage::crc32c(
                        std::string_view(bytes).substr(0, 192)),
                "header checksum should cover exactly the first 192 bytes");
        snowseek::test::require(bytes[196] == 0 && bytes[197] == 0 &&
                                        bytes[198] == 0 && bytes[199] == 0,
                                "final header reserved field should be zero");
}

/** @brief Verifies truncation and corrupted identity fields are rejected. */
void rejects_invalid_identity_and_size_fields() {
        const auto valid = serialize(make_header());
        require_rejected(valid.substr(0, valid.size() - 1),
                         "a truncated header should be rejected");

        auto invalid_magic = valid;
        invalid_magic[0] = 'X';
        require_rejected(invalid_magic, "invalid magic should be rejected");

        auto invalid_version = valid;
        set_u32(invalid_version, 8, 2);
        refresh_checksum(invalid_version);
        require_rejected(invalid_version,
                         "unsupported versions should be rejected");

        auto invalid_flags = valid;
        set_u32(invalid_flags, 12, 0x80000000U);
        refresh_checksum(invalid_flags);
        require_rejected(invalid_flags,
                         "unknown feature flags should be rejected");

        auto invalid_header_size = valid;
        set_u32(invalid_header_size, 16, 199);
        refresh_checksum(invalid_header_size);
        require_rejected(invalid_header_size,
                         "unknown header sizes should be rejected");

        auto invalid_section_count = valid;
        set_u32(invalid_section_count, 20, 4);
        refresh_checksum(invalid_section_count);
        require_rejected(invalid_section_count,
                         "unknown section counts should be rejected");
}

/** @brief Verifies CRC and every reserved field are enforced. */
void rejects_checksum_and_reserved_field_corruption() {
        const auto valid = serialize(make_header());

        auto invalid_checksum = valid;
        invalid_checksum[192] = static_cast<char>(
                static_cast<unsigned char>(invalid_checksum[192]) ^ 1U);
        require_rejected(invalid_checksum,
                         "an incorrect header checksum should be rejected");

        auto header_reserved = valid;
        set_u32(header_reserved, 196, 1);
        require_rejected(header_reserved,
                         "a nonzero header reserved field should be rejected");

        auto section_flags = valid;
        set_u32(section_flags, 36, 1);
        refresh_checksum(section_flags);
        require_rejected(section_flags,
                         "nonzero section flags should be rejected");

        auto section_reserved = valid;
        set_u32(section_reserved, 60, 1);
        refresh_checksum(section_reserved);
        require_rejected(section_reserved,
                         "a nonzero section reserved field should be rejected");
}

/** @brief Verifies section identity, order, and uniqueness are enforced. */
void rejects_invalid_section_identity() {
        const auto valid = serialize(make_header());

        auto duplicate = valid;
        set_u32(duplicate, 64, 1);
        refresh_checksum(duplicate);
        require_rejected(duplicate,
                         "duplicate section kinds should be rejected");

        auto out_of_order = valid;
        set_u32(out_of_order, 32, 2);
        refresh_checksum(out_of_order);
        require_rejected(out_of_order,
                         "out-of-order sections should be rejected");
}

/** @brief Verifies gaps, overlaps, size mismatches, and overflow are rejected.
 */
void rejects_invalid_section_boundaries() {
        const auto header = make_header();
        const auto valid = serialize(header);

        auto gap = valid;
        set_u64(gap, 40, snowseek::storage::kIndexHeaderSize + 1U);
        refresh_checksum(gap);
        require_rejected(gap, "a gap before the first section should fail");

        auto overlap = valid;
        set_u64(overlap, 72, header.sections[1].offset - 1U);
        refresh_checksum(overlap);
        require_rejected(overlap, "overlapping sections should fail");

        auto wrong_file_size = valid;
        set_u64(wrong_file_size, 24, header.file_size + 1U);
        refresh_checksum(wrong_file_size);
        require_rejected(wrong_file_size,
                         "file size inconsistent with sections should fail");

        auto overflow = valid;
        set_u64(overflow, 48, std::numeric_limits<std::uint64_t>::max());
        refresh_checksum(overflow);
        require_rejected(overflow, "overflowing section bounds should fail");
}

/** @brief Verifies feature-dependent and empty-section invariants. */
void rejects_invalid_section_features() {
        auto positions_without_feature = make_header();
        positions_without_feature.feature_flags = 0;
        snowseek::test::require_throws<std::runtime_error>(
                [&positions_without_feature] {
                        std::ostringstream output;
                        snowseek::storage::write_header(
                                output, positions_without_feature);
                },
                "positions data without its feature should be rejected");

        snowseek::storage::IndexHeader empty;
        empty.sections[0].checksum = 1;
        snowseek::test::require_throws<std::runtime_error>(
                [&empty] {
                        std::ostringstream output;
                        snowseek::storage::write_header(output, empty);
                },
                "an empty section with a checksum should be rejected");
}

} // namespace

/** @brief Runs the index-header unit-test suite. */
int main() {
        return snowseek::test::run({
                {"round-trips the index header", round_trips_header},
                {"writes stable header layout", writes_stable_header_layout},
                {"rejects invalid identity and size fields",
                 rejects_invalid_identity_and_size_fields},
                {"rejects checksum and reserved field corruption",
                 rejects_checksum_and_reserved_field_corruption},
                {"rejects invalid section identity",
                 rejects_invalid_section_identity},
                {"rejects invalid section boundaries",
                 rejects_invalid_section_boundaries},
                {"rejects invalid section features",
                 rejects_invalid_section_features},
        });
}
