#include "snowseek/document/document_store.hpp"
#include "snowseek/index/index.hpp"
#include "snowseek/storage/index_file.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TemporaryDirectory {
      public:
        /** @brief Creates a unique directory for persistent-index fixtures. */
        TemporaryDirectory() {
                const auto seed = std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count();
                path_ = std::filesystem::temp_directory_path() /
                        ("snowseek-index-file-test-" + std::to_string(seed));
                std::filesystem::create_directory(path_);
        }

        /** @brief Removes all persistent-index fixtures. */
        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        /** @brief Returns the fixture directory path. */
        [[nodiscard]] const std::filesystem::path &path() const {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

/** @brief Reads complete binary fixture bytes. */
[[nodiscard]] std::string read_bytes(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
}

/** @brief Writes complete binary fixture bytes. */
void write_bytes(const std::filesystem::path &path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
                throw std::runtime_error("failed to write index fixture");
        }
}

/** @brief Builds a deterministic two-document persistent-index fixture. */
void make_index(snowseek::document::DocumentStore &documents,
                snowseek::index::InMemoryIndex &index) {
        const auto first = documents.add("a.txt", 12, -10);
        documents.set_token_count(first, 3);
        const auto second = documents.add(
                std::filesystem::path(std::u8string(u8"目录/文档.txt")), 8,
                20);
        documents.set_token_count(second, 1);
        index.add_occurrence("retry", first, 0);
        index.add_occurrence("retry", first, 2);
        index.add_occurrence("retry", second, 1);
        index.add_occurrence("timeout", first, 1);
}

/** @brief Verifies deterministic serialization and complete logical reload. */
void round_trips_complete_index() {
        const TemporaryDirectory temporary;
        snowseek::document::DocumentStore documents;
        snowseek::index::InMemoryIndex index;
        make_index(documents, index);
        const auto first = temporary.path() / "first.idx";
        const auto second = temporary.path() / "second.idx";
        const auto stats =
                snowseek::storage::write_index_file(first, documents, index);
        static_cast<void>(
                snowseek::storage::write_index_file(second, documents, index));

        snowseek::test::require_equal(read_bytes(first), read_bytes(second),
                                      "serialization should be deterministic");
        const auto loaded = snowseek::storage::read_index_file(first);
        snowseek::test::require_equal(loaded.documents.size(), std::size_t{2},
                                      "both documents should reload");
        snowseek::test::require_equal(loaded.documents.get(0).path,
                                      std::filesystem::path("a.txt"),
                                      "relative paths should round-trip");
        snowseek::test::require_equal(loaded.documents.get(0).modified_time_ns,
                                      std::int64_t{-10},
                                      "signed timestamps should round-trip");
        snowseek::test::require_equal(
                loaded.documents.get(1).path,
                std::filesystem::path(std::u8string(u8"目录/文档.txt")),
                "generic UTF-8 paths should round-trip");
        const auto *retry = loaded.index.find("retry");
        snowseek::test::require(retry != nullptr && retry->size() == 2,
                                "posting lists should reload");
        snowseek::test::require_equal((*retry)[0].positions,
                                      std::vector<snowseek::index::Position>{0,
                                                                             2},
                                      "positions should round-trip");
        snowseek::test::require_equal(stats.document_count, std::uint64_t{2},
                                      "document statistics should match");
        snowseek::test::require_equal(stats.term_count, std::uint64_t{2},
                                      "term statistics should match");
        snowseek::test::require_equal(stats.posting_count, std::uint64_t{3},
                                      "posting statistics should match");
        snowseek::test::require_equal(stats.position_count, std::uint64_t{4},
                                      "position statistics should match");
}

/** @brief Verifies paths outside valid UTF-8 cannot enter the portable format. */
void rejects_non_utf8_paths() {
        const TemporaryDirectory temporary;
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

/** @brief Verifies CRC corruption and truncation are rejected. */
void rejects_corrupted_or_truncated_files() {
        const TemporaryDirectory temporary;
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

        const auto truncated_path = temporary.path() / "truncated.idx";
        write_bytes(truncated_path,
                    std::string_view(original).substr(0, original.size() - 1));
        snowseek::test::require_throws<std::runtime_error>(
                [&truncated_path] {
                        static_cast<void>(snowseek::storage::read_index_file(
                                truncated_path));
                },
                "truncated files should be rejected");
}

} // namespace

/** @brief Runs persistent-index file integration tests. */
int main() {
        return snowseek::test::run({
                {"round-trips a complete index", round_trips_complete_index},
                {"rejects corrupted or truncated files",
                 rejects_corrupted_or_truncated_files},
                {"rejects non-UTF-8 paths", rejects_non_utf8_paths},
        });
}
