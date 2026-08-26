#include "snowseek/storage/index_manifest.hpp"

#include "snowseek/storage/binary_codec.hpp"
#include "snowseek/storage/checksum.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace snowseek::storage {
namespace {

constexpr std::array<char, 8> kManifestMagic{'S', 'N', 'O', 'W',
                                              'M', 'N', 'F', 'T'};
constexpr std::size_t kChecksummedHeaderSize = 52;
constexpr std::uint32_t kManifestFlags = 0;

/** @brief Validates semantic invariants shared by encoding and decoding. */
void validate_manifest(const IndexManifest &manifest) {
        if (manifest.generation == 0) {
                throw std::runtime_error("manifest generation must be nonzero");
        }
        if (manifest.active_segments.size() != 1) {
                throw std::runtime_error(
                        "Manifest v1 requires exactly one active Segment");
        }
        const auto active_segment = manifest.active_segments.front();
        if (active_segment == 0) {
                throw std::runtime_error("manifest SegmentId must be nonzero");
        }
        if (manifest.next_segment_id <= active_segment) {
                throw std::runtime_error(
                        "manifest next SegmentId must exceed active SegmentIds");
        }
}

/** @brief Serializes the fixed fields preceding the header checksum. */
[[nodiscard]] std::string encode_header_prefix(
        const IndexManifest &manifest, std::uint64_t payload_length,
        std::uint32_t payload_checksum) {
        std::ostringstream output(std::ios::out | std::ios::binary);
        output.write(kManifestMagic.data(),
                     static_cast<std::streamsize>(kManifestMagic.size()));
        write_u32_le(output, kManifestFormatVersion);
        write_u32_le(output, kManifestFlags);
        write_u32_le(output, kManifestHeaderSize);
        write_u32_le(output,
                     static_cast<std::uint32_t>(manifest.active_segments.size()));
        write_u64_le(output, manifest.generation);
        write_u64_le(output, manifest.next_segment_id);
        write_u64_le(output, payload_length);
        write_u32_le(output, payload_checksum);
        auto bytes = output.str();
        if (bytes.size() != kChecksummedHeaderSize) {
                throw std::runtime_error("internal Manifest header size mismatch");
        }
        return bytes;
}

} // namespace

std::string segment_file_name(SegmentId segment_id) {
        if (segment_id == 0) {
                throw std::invalid_argument("SegmentId must be nonzero");
        }
        std::ostringstream name;
        name << "segment-" << std::setw(16) << std::setfill('0') << segment_id
             << ".idx";
        return name.str();
}

std::string encode_manifest(const IndexManifest &manifest) {
        validate_manifest(manifest);
        std::ostringstream payload_stream(std::ios::out | std::ios::binary);
        write_u64_le(payload_stream, manifest.active_segments.front());
        const auto payload = payload_stream.str();
        const auto prefix = encode_header_prefix(
                manifest, static_cast<std::uint64_t>(payload.size()),
                crc32c(payload));

        std::string bytes;
        bytes.reserve(kManifestHeaderSize + payload.size());
        bytes.append(prefix);
        std::ostringstream suffix(std::ios::out | std::ios::binary);
        write_u32_le(suffix, crc32c(prefix));
        write_u64_le(suffix, 0);
        bytes.append(suffix.str());
        bytes.append(payload);
        if (bytes.size() != kManifestHeaderSize + payload.size()) {
                throw std::runtime_error("internal Manifest size mismatch");
        }
        return bytes;
}

IndexManifest decode_manifest(std::string_view bytes) {
        if (bytes.size() < kManifestHeaderSize) {
                throw std::runtime_error("truncated Manifest header");
        }
        std::istringstream input(
                std::string(bytes.substr(0, kManifestHeaderSize)),
                std::ios::in | std::ios::binary);
        std::array<char, kManifestMagic.size()> magic{};
        input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!input || magic != kManifestMagic) {
                throw std::runtime_error("invalid SnowSeek Manifest magic");
        }
        const auto version = read_u32_le(input);
        const auto flags = read_u32_le(input);
        const auto header_size = read_u32_le(input);
        const auto active_count = read_u32_le(input);
        IndexManifest manifest;
        manifest.generation = read_u64_le(input);
        manifest.next_segment_id = read_u64_le(input);
        const auto payload_length = read_u64_le(input);
        const auto payload_checksum = read_u32_le(input);
        const auto header_checksum = read_u32_le(input);
        const auto reserved = read_u64_le(input);

        if (version != kManifestFormatVersion) {
                throw std::runtime_error("unsupported Manifest version");
        }
        if (flags != 0 || reserved != 0) {
                throw std::runtime_error("Manifest reserved field is nonzero");
        }
        if (header_size != kManifestHeaderSize) {
                throw std::runtime_error("unsupported Manifest header size");
        }
        if (active_count != 1) {
                throw std::runtime_error(
                        "Manifest v1 requires exactly one active Segment");
        }
        if (payload_length != static_cast<std::uint64_t>(active_count) * 8) {
                throw std::runtime_error("Manifest payload length is inconsistent");
        }
        if (bytes.size() != kManifestHeaderSize + payload_length) {
                throw std::runtime_error("Manifest file length is inconsistent");
        }
        const auto prefix = bytes.substr(0, kChecksummedHeaderSize);
        if (crc32c(prefix) != header_checksum) {
                throw std::runtime_error("Manifest header checksum mismatch");
        }
        const auto payload = bytes.substr(kManifestHeaderSize);
        if (crc32c(payload) != payload_checksum) {
                throw std::runtime_error("Manifest payload checksum mismatch");
        }
        std::istringstream payload_input(std::string(payload),
                                         std::ios::in | std::ios::binary);
        manifest.active_segments.push_back(read_u64_le(payload_input));
        validate_manifest(manifest);
        return manifest;
}

IndexManifest read_manifest_file(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
                throw std::runtime_error("failed to open Manifest: " +
                                         path.string());
        }
        constexpr std::size_t expected_size =
                kManifestHeaderSize + sizeof(SegmentId);
        std::string bytes(expected_size + 1, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto bytes_read = input.gcount();
        if (input.bad()) {
                throw std::runtime_error("failed to read Manifest: " +
                                         path.string());
        }
        if (bytes_read != static_cast<std::streamsize>(expected_size)) {
                throw std::runtime_error(
                        "Manifest file length is inconsistent");
        }
        bytes.resize(expected_size);
        return decode_manifest(bytes);
}

} // namespace snowseek::storage
