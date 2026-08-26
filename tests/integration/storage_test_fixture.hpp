#pragma once

#include "document/document_store.hpp"
#include "index/index.hpp"
#include "storage/checksum.hpp"
#include "storage/index_file.hpp"
#include "storage/index_header.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace snowseek::test::storage_fixture {

/** @brief Owns a unique directory for one storage integration-test scope. */
class TemporaryDirectory {
      public:
        /**
         * @brief Creates a unique temporary directory with a readable prefix.
         * @param suite_name Short fixture-suite identifier used in the path.
         */
        explicit TemporaryDirectory(std::string_view suite_name) {
                const auto seed = std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count();
                path_ = std::filesystem::temp_directory_path() /
                        ("snowseek-" + std::string(suite_name) + "-" +
                         std::to_string(seed));
                std::filesystem::create_directory(path_);
        }

        /** @brief Removes every fixture owned by this scope. */
        ~TemporaryDirectory() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;
        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        /** @brief Returns the owned directory path. */
        [[nodiscard]] const std::filesystem::path &path() const noexcept {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

/**
 * @brief Reads an entire binary fixture.
 * @param path Fixture file to read.
 * @return Exact bytes stored in the file.
 */
[[nodiscard]] inline std::string read_bytes(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
}

/**
 * @brief Replaces a fixture file with exact bytes.
 * @param path Destination file.
 * @param bytes Complete binary contents.
 * @throws std::runtime_error if the write cannot be completed.
 */
inline void write_bytes(const std::filesystem::path &path,
                        std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
                throw std::runtime_error("failed to write storage fixture");
        }
}

/**
 * @brief Replaces one little-endian u32 in mutable Segment bytes.
 * @param bytes Complete mutable Segment contents.
 * @param offset Byte offset of the u32 field.
 * @param value Replacement value.
 */
inline void set_u32(std::string &bytes, std::size_t offset,
                    std::uint32_t value) {
        for (unsigned int byte = 0; byte < 4; ++byte) {
                bytes[offset + byte] =
                        static_cast<char>((value >> (byte * 8U)) & 0xffU);
        }
}

/**
 * @brief Repairs one section descriptor and the enclosing Header CRC.
 * @param bytes Complete mutable Segment contents.
 * @param header Header decoded before the section mutation.
 * @param section_index Section whose payload was changed.
 */
inline void
refresh_section_checksums(std::string &bytes,
                          const snowseek::storage::IndexHeader &header,
                          std::size_t section_index) {
        const auto &section = header.sections[section_index];
        const auto checksum =
                snowseek::storage::crc32c(std::string_view(bytes).substr(
                        static_cast<std::size_t>(section.offset),
                        static_cast<std::size_t>(section.length)));
        set_u32(bytes, 32 + section_index * 32 + 24, checksum);
        set_u32(bytes, 192,
                snowseek::storage::crc32c(
                        std::string_view(bytes).substr(0, 192)));
}

/**
 * @brief Builds a deterministic two-document in-memory index fixture.
 * @param documents Destination document table.
 * @param index Destination inverted index.
 */
inline void make_index(snowseek::document::DocumentStore &documents,
                       snowseek::index::InMemoryIndex &index) {
        const auto first = documents.add("a.txt", 12, -10, 0x12345678U);
        documents.set_token_count(first, 3);
        const auto second = documents.add(
                std::filesystem::path(std::u8string(u8"目录/文档.txt")), 8, 20,
                0xabcdef01U);
        documents.set_token_count(second, 1);
        index.add_occurrence("retry", first, 0);
        index.add_occurrence("retry", first, 2);
        index.add_occurrence("retry", second, 1);
        index.add_occurrence("timeout", first, 1);
}

/**
 * @brief Converts one writer-produced v2 fixture to the retired v1 layout.
 * @param path Existing valid v2 Segment.
 * @return Complete v1 bytes with 40-byte live Document records.
 */
[[nodiscard]] inline std::string
make_legacy_v1_bytes(const std::filesystem::path &path) {
        const auto bytes = read_bytes(path);
        std::istringstream header_input(bytes, std::ios::in | std::ios::binary);
        auto header = snowseek::storage::read_header(header_input);
        std::array<std::string, snowseek::storage::kIndexSectionCount> sections;
        for (std::size_t index = 0; index < sections.size(); ++index) {
                sections[index] = bytes.substr(
                        static_cast<std::size_t>(header.sections[index].offset),
                        static_cast<std::size_t>(
                                header.sections[index].length));
        }
        const auto &v2_documents = sections[0];
        if (v2_documents.size() < 8 || (v2_documents.size() - 8) % 48 != 0) {
                throw std::runtime_error("invalid v2 Document fixture");
        }
        const auto count = (v2_documents.size() - 8) / 48;
        std::string v1_documents = v2_documents.substr(0, 8);
        v1_documents.reserve(8 + count * 40);
        for (std::size_t index = 0; index < count; ++index) {
                v1_documents.append(v2_documents, 8 + index * 48, 36);
                v1_documents.append(4, '\0');
        }
        sections[0] = std::move(v1_documents);

        std::uint64_t offset = snowseek::storage::kIndexHeaderSize;
        for (std::size_t index = 0; index < sections.size(); ++index) {
                header.sections[index].offset = offset;
                header.sections[index].length = sections[index].size();
                header.sections[index].checksum =
                        snowseek::storage::crc32c(sections[index]);
                offset += sections[index].size();
        }
        header.file_size = offset;
        std::ostringstream output(std::ios::out | std::ios::binary);
        snowseek::storage::write_header(output, header);
        for (const auto &section : sections) {
                output.write(section.data(),
                             static_cast<std::streamsize>(section.size()));
        }
        auto legacy = output.str();
        set_u32(legacy, 8, snowseek::storage::kLegacyIndexFormatVersion);
        set_u32(legacy, 192,
                snowseek::storage::crc32c(
                        std::string_view(legacy).substr(0, 192)));
        return legacy;
}

/**
 * @brief Creates a valid empty fixed-name Segment without a Manifest.
 * @param directory Directory that receives the legacy layout.
 */
inline void write_legacy_segment(const std::filesystem::path &directory) {
        std::filesystem::create_directories(directory);
        static_cast<void>(snowseek::storage::write_index_file(
                directory / snowseek::storage::kSegmentFileName,
                snowseek::document::DocumentStore{},
                snowseek::index::InMemoryIndex{}));
}

} // namespace snowseek::test::storage_fixture
