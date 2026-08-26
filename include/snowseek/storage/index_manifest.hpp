#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::storage {

using SegmentId = std::uint64_t;

inline constexpr const char *kManifestFileName = "MANIFEST";
inline constexpr std::uint32_t kManifestFormatVersion = 1;
inline constexpr std::uint32_t kManifestHeaderSize = 64;

struct IndexManifest {
        std::uint64_t generation{};
        SegmentId next_segment_id{};
        std::vector<SegmentId> active_segments;
};

/**
 * @brief Derives the stable on-disk filename for a nonzero SegmentId.
 * @param segment_id Monotonic identifier assigned at publication time.
 * @return Decimal filename with at least sixteen zero-padded digits.
 * @throws std::invalid_argument If segment_id is zero.
 */
[[nodiscard]] std::string segment_file_name(SegmentId segment_id);

/**
 * @brief Encodes and checksums one single-Segment Manifest v1.
 * @param manifest Generation, next identifier, and active SegmentId.
 * @return Complete little-endian Manifest bytes.
 * @throws std::runtime_error If a Manifest invariant is violated.
 */
[[nodiscard]] std::string encode_manifest(const IndexManifest &manifest);

/**
 * @brief Decodes and fully validates Manifest v1 bytes.
 * @param bytes Complete file contents, including header and payload.
 * @return Validated Manifest values.
 * @throws std::runtime_error If bytes are malformed, unsupported, or corrupt.
 */
[[nodiscard]] IndexManifest decode_manifest(std::string_view bytes);

/**
 * @brief Reads and validates a complete Manifest v1 file.
 * @param path Manifest path to read.
 * @return Validated Manifest values.
 * @throws std::runtime_error If the file cannot be read or is invalid.
 */
[[nodiscard]] IndexManifest
read_manifest_file(const std::filesystem::path &path);

} // namespace snowseek::storage
