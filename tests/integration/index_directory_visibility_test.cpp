/**
 * @file index_directory_visibility_test.cpp
 * @brief Verifies reader visibility rules for manifest-backed index
 * directories.
 */

#include "storage/index_file.hpp"
#include "storage/index_manifest.hpp"

#include "storage_test_fixture.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace {

using snowseek::test::storage_fixture::TemporaryDirectory;
using snowseek::test::storage_fixture::write_bytes;
using snowseek::test::storage_fixture::write_legacy_segment;

/** @brief Verifies Manifest-free readers reject legacy directory layouts. */
void rejects_legacy_and_manifest_errors() {
        const TemporaryDirectory temporary("index-directory-visibility");
        const auto legacy = temporary.path() / "legacy";
        write_legacy_segment(legacy);
        try {
                static_cast<void>(
                        snowseek::storage::validate_index_directory(legacy));
                throw std::runtime_error(
                        "Manifest-free directory was unexpectedly accepted");
        } catch (const std::runtime_error &error) {
                snowseek::test::require(
                        std::string_view(error.what()).find("rebuild") !=
                                std::string_view::npos,
                        "a missing Manifest should request a rebuild");
        }

        write_bytes(legacy / snowseek::storage::kManifestFileName, "broken");
        snowseek::test::require_throws<std::runtime_error>(
                [&legacy] {
                        static_cast<void>(
                                snowseek::storage::read_index_directory(
                                        legacy));
                },
                "a corrupt Manifest must not fall back to the legacy Segment");

        const auto missing = temporary.path() / "missing";
        write_legacy_segment(missing);
        write_bytes(missing / snowseek::storage::kManifestFileName,
                    snowseek::storage::encode_manifest({1, 3, {2}}));
        snowseek::test::require_throws<std::runtime_error>(
                [&missing] {
                        static_cast<void>(
                                snowseek::storage::validate_index_directory(
                                        missing));
                },
                "a missing referenced Segment must not use the legacy file");

        const auto corrupt = temporary.path() / "corrupt";
        write_legacy_segment(corrupt);
        write_bytes(corrupt / snowseek::storage::segment_file_name(2),
                    "not a Segment");
        write_bytes(corrupt / snowseek::storage::kManifestFileName,
                    snowseek::storage::encode_manifest({1, 3, {2}}));
        snowseek::test::require_throws<std::runtime_error>(
                [&corrupt] {
                        static_cast<void>(
                                snowseek::storage::read_index_directory(
                                        corrupt));
                },
                "a corrupt active Segment must not use the legacy file");
}

/** @brief Verifies all active Segments must share the Positions capability. */
void rejects_mixed_segment_capabilities() {
        const TemporaryDirectory temporary("index-directory-visibility");
        const auto index = temporary.path() / "mixed";
        std::filesystem::create_directory(index);
        static_cast<void>(snowseek::storage::write_index_file(
                index / snowseek::storage::segment_file_name(1),
                snowseek::document::DocumentStore{},
                snowseek::index::InMemoryIndex{true}));
        static_cast<void>(snowseek::storage::write_index_file(
                index / snowseek::storage::segment_file_name(2),
                snowseek::document::DocumentStore{},
                snowseek::index::InMemoryIndex{false}));
        write_bytes(index / snowseek::storage::kManifestFileName,
                    snowseek::storage::encode_manifest({1, 3, {1, 2}}));
        snowseek::test::require_throws<std::runtime_error>(
                [&index] {
                        static_cast<void>(
                                snowseek::storage::read_index_directory(index));
                },
                "active Segments with mixed Position flags should be rejected");
}

} // namespace

/** @brief Runs directory visibility and validation integration tests. */
int main() {
        return snowseek::test::run({
                {"rejects legacy and invalid Manifest directories",
                 rejects_legacy_and_manifest_errors},
                {"rejects mixed Segment capabilities",
                 rejects_mixed_segment_capabilities},
        });
}
