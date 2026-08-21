#include "snowseek/index/in_memory_index_builder.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

class TemporaryDirectory {
      public:
        TemporaryDirectory() {
                const auto seed = std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count();
                const auto base = std::filesystem::temp_directory_path();
                for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
                        path_ = base / ("snowseek-index-builder-test-" +
                                        std::to_string(seed) + "-" +
                                        std::to_string(attempt));
                        std::error_code error;
                        if (std::filesystem::create_directory(path_, error)) {
                                return;
                        }
                }
                throw std::runtime_error(
                        "failed to create a temporary test directory");
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;
        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        [[nodiscard]] const std::filesystem::path &path() const {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, std::string_view contents) {
        std::ofstream output(path, std::ios::binary);
        output.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        if (!output) {
                throw std::runtime_error("failed to write builder test file");
        }
}

void builds_documents_and_postings_from_a_directory() {
        const TemporaryDirectory temporary;
        const auto first = temporary.path() / "a.txt";
        const auto second = temporary.path() / "b.txt";
        const auto empty = temporary.path() / "empty.txt";
        write_file(first, "Timeout retry timeout");
        write_file(second, "retry policy");
        write_file(empty, "");

        snowseek::index::InMemoryBuildOptions options;
        options.read_options.chunk_size = 2;
        const auto result =
                snowseek::index::InMemoryIndexBuilder(options).build(
                        temporary.path());

        snowseek::test::require(result.scan_errors.empty(),
                                "the valid corpus should have no scan errors");
        snowseek::test::require(
                result.document_errors.empty(),
                "the valid corpus should have no document errors");
        snowseek::test::require_equal(result.documents.size(), std::size_t{3},
                                      "all files should become documents");
        snowseek::test::require_equal(
                result.documents.get(0).path, first,
                "sorted paths should determine the first document id");
        snowseek::test::require_equal(
                result.documents.get(1).path, second,
                "sorted paths should determine the second document id");
        snowseek::test::require_equal(
                result.documents.get(2).path, empty,
                "an empty file should still become a document");
        snowseek::test::require_equal(
                result.documents.get(0).token_count, std::uint32_t{3},
                "the first document should retain its token count");
        snowseek::test::require_equal(
                result.documents.get(2).token_count, std::uint32_t{0},
                "an empty document should have zero tokens");
        snowseek::test::require(
                result.documents.get(0).modified_time_ns > 0,
                "modification time should use Unix epoch nanoseconds");

        const auto *timeout = result.index.find("timeout");
        snowseek::test::require(timeout != nullptr,
                                "the timeout term should be indexed");
        snowseek::test::require_equal(timeout->size(), std::size_t{1},
                                      "timeout should occur in one document");
        snowseek::test::require_equal(
                (*timeout)[0].positions,
                std::vector<snowseek::index::Position>{0, 2},
                "timeout positions should be retained");

        const auto *retry = result.index.find("retry");
        snowseek::test::require(retry != nullptr,
                                "the retry term should be indexed");
        snowseek::test::require_equal(retry->size(), std::size_t{2},
                                      "retry should occur in two documents");
        snowseek::test::require_equal((*retry)[0].document_id,
                                      snowseek::document::DocumentId{0},
                                      "retry should first occur in document 0");
        snowseek::test::require_equal((*retry)[0].positions,
                                      std::vector<snowseek::index::Position>{1},
                                      "the first retry position should match");
        snowseek::test::require_equal((*retry)[1].document_id,
                                      snowseek::document::DocumentId{1},
                                      "retry should next occur in document 1");
        snowseek::test::require_equal((*retry)[1].positions,
                                      std::vector<snowseek::index::Position>{0},
                                      "positions should restart per document");

        snowseek::test::require_equal(result.stats.scanned_files,
                                      std::uint64_t{3},
                                      "stats should count scanned files");
        snowseek::test::require_equal(result.stats.indexed_files,
                                      std::uint64_t{3},
                                      "stats should count indexed files");
        snowseek::test::require_equal(result.stats.failed_files,
                                      std::uint64_t{0},
                                      "stats should report no failures");
        snowseek::test::require_equal(result.stats.token_count,
                                      std::uint64_t{5},
                                      "stats should count all tokens");
        snowseek::test::require_equal(result.stats.indexed_bytes,
                                      std::uint64_t{33},
                                      "stats should count source bytes");
}

void applies_scan_filters() {
        const TemporaryDirectory temporary;
        write_file(temporary.path() / "keep.txt", "kept");
        write_file(temporary.path() / "skip.txt", "skipped");
        write_file(temporary.path() / "source.cpp", "source");

        snowseek::index::InMemoryBuildOptions options;
        options.scan_options.include_patterns = {"*.txt"};
        options.scan_options.exclude_patterns = {"skip*"};
        const auto result =
                snowseek::index::InMemoryIndexBuilder(options).build(
                        temporary.path());

        snowseek::test::require_equal(result.documents.size(), std::size_t{1},
                                      "scan filters should select one file");
        snowseek::test::require(result.index.find("kept") != nullptr,
                                "the included file should be indexed");
        snowseek::test::require(result.index.find("skipped") == nullptr,
                                "the excluded file should not be indexed");
        snowseek::test::require(result.index.find("source") == nullptr,
                                "a non-included extension should be skipped");
}

void replaces_invalid_utf8_by_default() {
        const TemporaryDirectory temporary;
        std::string contents{"left "};
        contents.push_back(static_cast<char>(0xff));
        contents.append(" right");
        write_file(temporary.path() / "invalid.txt", contents);

        const auto result =
                snowseek::index::InMemoryIndexBuilder{}.build(temporary.path());

        snowseek::test::require(result.document_errors.empty(),
                                "replacement mode should keep the document");
        snowseek::test::require_equal(result.documents.size(), std::size_t{1},
                                      "the replaced file should be committed");
        snowseek::test::require(result.index.find("left") != nullptr,
                                "tokens before invalid UTF-8 should remain");
        snowseek::test::require(result.index.find("right") != nullptr,
                                "tokens after invalid UTF-8 should remain");
        snowseek::test::require_equal(
                result.documents.get(0).token_count, std::uint32_t{2},
                "replacement bytes should act as delimiters");
}

void does_not_commit_failed_documents() {
        const TemporaryDirectory temporary;
        write_file(temporary.path() / "a-valid.txt", "safe");

        std::string invalid{"part "};
        invalid.push_back(static_cast<char>(0xff));
        write_file(temporary.path() / "b-invalid.txt", invalid);
        write_file(temporary.path() / "c-too-long.txt", "temp abcde");
        write_file(temporary.path() / "d-valid.txt", "done");

        snowseek::index::InMemoryBuildOptions options;
        options.read_options.chunk_size = 1;
        options.read_options.invalid_utf8_policy =
                snowseek::document::InvalidUtf8Policy::reject;
        options.tokenizer_options.max_token_length = 4;
        const auto result =
                snowseek::index::InMemoryIndexBuilder(options).build(
                        temporary.path());

        snowseek::test::require_equal(
                result.documents.size(), std::size_t{2},
                "only fully parsed documents should be committed");
        snowseek::test::require_equal(
                result.document_errors.size(), std::size_t{2},
                "both failed documents should be diagnosed");
        snowseek::test::require_equal(
                result.documents.get(0).path, temporary.path() / "a-valid.txt",
                "the first successful file should receive document id zero");
        snowseek::test::require_equal(
                result.documents.get(1).path, temporary.path() / "d-valid.txt",
                "document ids should remain contiguous after failures");
        snowseek::test::require(result.index.find("safe") != nullptr,
                                "a successful file should be indexed");
        snowseek::test::require(result.index.find("done") != nullptr,
                                "processing should continue after failures");
        snowseek::test::require(result.index.find("part") == nullptr,
                                "an invalid UTF-8 file must not commit tokens");
        snowseek::test::require(
                result.index.find("temp") == nullptr,
                "an oversized-token file must not commit tokens");
        snowseek::test::require_equal(result.stats.scanned_files,
                                      std::uint64_t{4},
                                      "all candidate files should be counted");
        snowseek::test::require_equal(
                result.stats.indexed_files, std::uint64_t{2},
                "only successful files should be counted as indexed");
        snowseek::test::require_equal(result.stats.failed_files,
                                      std::uint64_t{2},
                                      "failed files should be counted");
}

void returns_scan_errors_for_a_missing_root() {
        const TemporaryDirectory temporary;
        const auto missing = temporary.path() / "missing";
        const auto result =
                snowseek::index::InMemoryIndexBuilder{}.build(missing);

        snowseek::test::require_equal(
                result.scan_errors.size(), std::size_t{1},
                "a missing root should produce one scan error");
        snowseek::test::require(
                result.document_errors.empty(),
                "no document should be opened after scan failure");
        snowseek::test::require_equal(result.stats.scanned_files,
                                      std::uint64_t{0},
                                      "a missing root has no candidate files");
}

} // namespace

int main() {
        return snowseek::test::run({
                {"builds documents and postings from a directory",
                 builds_documents_and_postings_from_a_directory},
                {"applies scan filters", applies_scan_filters},
                {"replaces invalid UTF-8 by default",
                 replaces_invalid_utf8_by_default},
                {"does not commit failed documents",
                 does_not_commit_failed_documents},
                {"returns scan errors for a missing root",
                 returns_scan_errors_for_a_missing_root},
        });
}
