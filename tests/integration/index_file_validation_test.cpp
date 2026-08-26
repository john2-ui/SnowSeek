/**
 * @file index_file_validation_test.cpp
 * @brief Exercises index-file compatibility checks and corruption diagnostics.
 */

#include "storage/index_file.hpp"
#include "storage/index_header.hpp"

#include "storage_test_fixture.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using snowseek::test::storage_fixture::make_index;
using snowseek::test::storage_fixture::make_legacy_v1_bytes;
using snowseek::test::storage_fixture::read_bytes;
using snowseek::test::storage_fixture::refresh_section_checksums;
using snowseek::test::storage_fixture::set_u32;
using snowseek::test::storage_fixture::TemporaryDirectory;
using snowseek::test::storage_fixture::write_bytes;

/** @brief Verifies retired v1 Segments request a complete rebuild. */
void rejects_legacy_v1_segments() {
        const TemporaryDirectory temporary("index-file-validation");
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index;
        make_index(documents, index);
        const auto v2 = temporary.path() / "source-v2.idx";
        static_cast<void>(
                snowseek::storage::write_index_file(v2, documents, index));
        const auto v1 = temporary.path() / "legacy-v1.idx";
        write_bytes(v1, make_legacy_v1_bytes(v2));

        for (const auto operation : {0, 1}) {
                try {
                        if (operation == 0) {
                                static_cast<void>(
                                        snowseek::storage::read_index_file(v1));
                        } else {
                                static_cast<void>(
                                        snowseek::storage::validate_index_file(
                                                v1));
                        }
                } catch (const std::runtime_error &error) {
                        snowseek::test::require(
                                std::string_view(error.what())
                                                .find("rebuild") !=
                                        std::string_view::npos,
                                "v1 rejection should tell callers to rebuild");
                        continue;
                }
                throw std::runtime_error("v1 Segment should be rejected");
        }
}

/** @brief Verifies v2 flags, CRC validity, and reserved fields are strict. */
void rejects_invalid_v2_document_metadata() {
        const TemporaryDirectory temporary("index-file-validation");
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index;
        make_index(documents, index);
        const auto valid = temporary.path() / "valid-v2.idx";
        static_cast<void>(
                snowseek::storage::write_index_file(valid, documents, index));
        const auto original = read_bytes(valid);
        std::istringstream input(original, std::ios::in | std::ios::binary);
        const auto header = snowseek::storage::read_header(input);
        const auto first_record =
                static_cast<std::size_t>(header.sections[0].offset + 8);

        auto unknown_flags = original;
        set_u32(unknown_flags, first_record + 36, 0x80000000U);
        refresh_section_checksums(unknown_flags, header, 0);
        const auto unknown_path = temporary.path() / "unknown-flags.idx";
        write_bytes(unknown_path, unknown_flags);
        snowseek::test::require_throws<std::runtime_error>(
                [&unknown_path] {
                        static_cast<void>(
                                snowseek::storage::validate_index_file(
                                        unknown_path));
                },
                "unknown v2 Document flags should be rejected");

        auto invalid_crc = original;
        set_u32(invalid_crc, first_record + 36, 0);
        refresh_section_checksums(invalid_crc, header, 0);
        const auto crc_path = temporary.path() / "invalid-crc-metadata.idx";
        write_bytes(crc_path, invalid_crc);
        snowseek::test::require_throws<std::runtime_error>(
                [&crc_path] {
                        static_cast<void>(
                                snowseek::storage::read_index_file(crc_path));
                },
                "a nonzero CRC without its validity bit should be rejected");

        auto reserved = original;
        set_u32(reserved, first_record + 44, 1);
        refresh_section_checksums(reserved, header, 0);
        const auto reserved_path = temporary.path() / "reserved.idx";
        write_bytes(reserved_path, reserved);
        snowseek::test::require_throws<std::runtime_error>(
                [&reserved_path] {
                        static_cast<void>(
                                snowseek::storage::validate_index_file(
                                        reserved_path));
                },
                "nonzero v2 Document reserved fields should be rejected");
}

/** @brief Verifies valid CRCs cannot mask malformed logical records. */
void rejects_checksum_valid_logical_corruption() {
        const TemporaryDirectory temporary("index-file-validation");
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index;
        make_index(documents, index);
        const auto valid = temporary.path() / "valid-logical.idx";
        static_cast<void>(
                snowseek::storage::write_index_file(valid, documents, index));
        const auto original = read_bytes(valid);
        std::istringstream input(original, std::ios::in | std::ios::binary);
        const auto header = snowseek::storage::read_header(input);

        auto invalid_term = original;
        set_u32(invalid_term,
                static_cast<std::size_t>(header.sections[2].offset + 16), 0);
        refresh_section_checksums(invalid_term, header, 2);
        const auto term_path = temporary.path() / "invalid-term.idx";
        write_bytes(term_path, invalid_term);

        auto invalid_posting = original;
        set_u32(invalid_posting,
                static_cast<std::size_t>(header.sections[3].offset + 4), 0);
        refresh_section_checksums(invalid_posting, header, 3);
        const auto posting_path = temporary.path() / "invalid-posting.idx";
        write_bytes(posting_path, invalid_posting);

        auto invalid_position = original;
        set_u32(invalid_position,
                static_cast<std::size_t>(header.sections[4].offset + 4), 0);
        refresh_section_checksums(invalid_position, header, 4);
        const auto position_path = temporary.path() / "invalid-position.idx";
        write_bytes(position_path, invalid_position);

        for (const auto &path : {term_path, posting_path, position_path}) {
                snowseek::test::require_throws<std::runtime_error>(
                        [&path] {
                                static_cast<void>(
                                        snowseek::storage::validate_index_file(
                                                path));
                        },
                        "checksum-valid logical corruption should be rejected");
        }
}

/** @brief Verifies paths outside valid UTF-8 cannot enter the portable format.
 */
void rejects_non_utf8_paths() {
        const TemporaryDirectory temporary("index-file-validation");
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index;
        const auto document = documents.add(
                std::filesystem::path(std::string("invalid-\xff.txt", 13)), 1,
                0);
        documents.set_token_count(document, 1);
        index.add_occurrence("invalid", document, 0);

        snowseek::test::require_throws<std::runtime_error>(
                [&temporary, &documents, &index] {
                        static_cast<void>(snowseek::storage::write_index_file(
                                temporary.path() / "invalid.idx", documents,
                                index));
                },
                "non-UTF-8 paths should be rejected");
}

/** @brief Verifies persisted source paths cannot escape their source root. */
void rejects_nonrelative_paths() {
        const TemporaryDirectory temporary("index-file-validation");
        for (const auto &path : {std::filesystem::path("../escape.txt"),
                                 std::filesystem::path("/absolute.txt")}) {
                snowseek::document::DocumentStore documents;
                snowseek::index::InMemoryIndex index;
                const auto document = documents.add(path, 1, 0, 1);
                documents.set_token_count(document, 1);
                index.add_occurrence("invalid", document, 0);
                snowseek::test::require_throws<std::runtime_error>(
                        [&temporary, &documents, &index] {
                                static_cast<void>(
                                        snowseek::storage::write_index_file(
                                                temporary.path() /
                                                        "nonrelative.idx",
                                                documents, index));
                        },
                        "persistent paths should be normalized and relative");
        }
}

/** @brief Verifies CRC corruption and truncation are rejected. */
void rejects_corrupted_or_truncated_files() {
        const TemporaryDirectory temporary("index-file-validation");
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index;
        make_index(documents, index);
        const auto valid = temporary.path() / "valid.idx";
        static_cast<void>(
                snowseek::storage::write_index_file(valid, documents, index));
        const auto original = read_bytes(valid);

        auto corrupted = original;
        corrupted.back() = static_cast<char>(
                static_cast<unsigned char>(corrupted.back()) ^ 1U);
        const auto corrupted_path = temporary.path() / "corrupted.idx";
        write_bytes(corrupted_path, corrupted);
        snowseek::test::require_throws<std::runtime_error>(
                [&corrupted_path] {
                        static_cast<void>(snowseek::storage::read_index_file(
                                corrupted_path));
                },
                "section corruption should be rejected");
        snowseek::test::require_throws<std::runtime_error>(
                [&corrupted_path] {
                        static_cast<void>(
                                snowseek::storage::validate_index_file(
                                        corrupted_path));
                },
                "streaming validation should reject section corruption");

        const auto truncated_path = temporary.path() / "truncated.idx";
        write_bytes(truncated_path,
                    std::string_view(original).substr(0, original.size() - 1));
        snowseek::test::require_throws<std::runtime_error>(
                [&truncated_path] {
                        static_cast<void>(snowseek::storage::read_index_file(
                                truncated_path));
                },
                "truncated files should be rejected");
        snowseek::test::require_throws<std::runtime_error>(
                [&truncated_path] {
                        static_cast<void>(
                                snowseek::storage::validate_index_file(
                                        truncated_path));
                },
                "streaming validation should reject truncation");
}

} // namespace

/** @brief Runs persistent Segment validation integration tests. */
int main() {
        return snowseek::test::run({
                {"rejects legacy v1 Segments", rejects_legacy_v1_segments},
                {"rejects invalid v2 Document metadata",
                 rejects_invalid_v2_document_metadata},
                {"rejects checksum-valid logical corruption",
                 rejects_checksum_valid_logical_corruption},
                {"rejects corrupted or truncated files",
                 rejects_corrupted_or_truncated_files},
                {"rejects non-UTF-8 paths", rejects_non_utf8_paths},
                {"rejects nonrelative paths", rejects_nonrelative_paths},
        });
}
