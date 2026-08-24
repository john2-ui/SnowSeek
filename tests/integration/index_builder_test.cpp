#include "snowseek/index/index_builder.hpp"
#include "snowseek/storage/index_file.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

class TemporaryDirectory {
      public:
        /** @brief Creates a unique temporary directory for one test scope. */
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

        /** @brief Removes the temporary directory and its contents. */
        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        /** @brief Returns the temporary directory path. */
        [[nodiscard]] const std::filesystem::path &path() const {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

/**
 * @brief Writes binary fixture contents to a path.
 * @param path Destination fixture path.
 * @param contents Bytes to write.
 * @throws std::runtime_error If the fixture cannot be written.
 */
void write_file(const std::filesystem::path &path, std::string_view contents) {
        std::ofstream output(path, std::ios::binary);
        output.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        if (!output) {
                throw std::runtime_error("failed to write builder test file");
        }
}

/**
 * @brief Reads a complete Segment fixture for deterministic comparison.
 * @param path Existing Segment path.
 * @return Complete binary contents.
 */
[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
}

/** @brief Verifies end-to-end document and posting construction. */
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

/** @brief Verifies propagation of scanner include and exclude filters. */
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

/** @brief Verifies default replacement of invalid UTF-8 during indexing. */
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

/** @brief Verifies atomic rejection and continuation after file failures. */
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

/** @brief Verifies scan diagnostics for a missing corpus root. */
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

/** @brief Verifies classified memory estimates and repeatability. */
void reports_deterministic_memory_estimates() {
        const TemporaryDirectory temporary;
        write_file(temporary.path() / "a.txt", "alpha beta alpha");
        write_file(temporary.path() / "b.txt", "beta gamma");

        snowseek::index::InMemoryBuildOptions options;
        options.read_options.chunk_size = 2;
        const snowseek::index::InMemoryIndexBuilder builder(options);
        const auto first = builder.build(temporary.path());
        const auto second = builder.build(temporary.path());

        const auto &memory = first.stats.memory;
        snowseek::test::require(
                memory.metadata_bytes > 0 && memory.reader_peak_bytes > 0 &&
                        memory.token_peak_bytes > 0 &&
                        memory.dictionary_bytes > 0 && memory.posting_bytes > 0,
                "a nonempty corpus should populate every memory category");
        snowseek::test::require_equal(
                memory.estimated_peak_bytes,
                memory.metadata_bytes + memory.reader_peak_bytes +
                        memory.token_peak_bytes + memory.dictionary_bytes +
                        memory.posting_bytes,
                "the peak estimate should sum all classified bytes");
        const auto &repeated = second.stats.memory;
        snowseek::test::require(
                memory.metadata_bytes == repeated.metadata_bytes &&
                        memory.reader_peak_bytes ==
                                repeated.reader_peak_bytes &&
                        memory.token_peak_bytes == repeated.token_peak_bytes &&
                        memory.dictionary_bytes == repeated.dictionary_bytes &&
                        memory.posting_bytes == repeated.posting_bytes &&
                        memory.estimated_peak_bytes ==
                                repeated.estimated_peak_bytes,
                "memory estimates should be stable across repeated builds");
}

/** @brief Verifies an empty corpus reports no retained build memory. */
void reports_zero_memory_for_an_empty_corpus() {
        const TemporaryDirectory temporary;
        const auto result =
                snowseek::index::InMemoryIndexBuilder{}.build(temporary.path());
        const auto &memory = result.stats.memory;
        snowseek::test::require_equal(memory.metadata_bytes, std::uint64_t{0},
                                      "empty metadata should estimate zero");
        snowseek::test::require_equal(memory.reader_peak_bytes,
                                      std::uint64_t{0},
                                      "an unused reader should estimate zero");
        snowseek::test::require_equal(
                memory.token_peak_bytes, std::uint64_t{0},
                "empty token storage should estimate zero");
        snowseek::test::require_equal(
                memory.dictionary_bytes, std::uint64_t{0},
                "an empty dictionary should estimate zero");
        snowseek::test::require_equal(memory.posting_bytes, std::uint64_t{0},
                                      "empty postings should estimate zero");
        snowseek::test::require_equal(
                memory.estimated_peak_bytes, std::uint64_t{0},
                "an empty build should estimate zero total bytes");
}

/** @brief Verifies failed parsing still contributes transient memory peaks. */
void counts_failed_document_buffers() {
        const TemporaryDirectory temporary;
        write_file(temporary.path() / "too-long.txt", "oversized");

        snowseek::index::InMemoryBuildOptions options;
        options.read_options.chunk_size = 2;
        options.tokenizer_options.max_token_length = 4;
        const auto result =
                snowseek::index::InMemoryIndexBuilder(options).build(
                        temporary.path());

        snowseek::test::require_equal(
                result.stats.failed_files, std::uint64_t{1},
                "the oversized token should fail the only document");
        snowseek::test::require(result.stats.memory.reader_peak_bytes > 0 &&
                                        result.stats.memory.token_peak_bytes >
                                                0,
                                "failed parsing should retain transient peaks");
        snowseek::test::require_equal(
                result.stats.memory.dictionary_bytes, std::uint64_t{0},
                "failed documents should not allocate dictionary entries");
        snowseek::test::require_equal(
                result.stats.memory.posting_bytes, std::uint64_t{0},
                "failed documents should not allocate postings");
}

/** @brief Verifies positive persistent Segment flush configuration. */
void rejects_zero_segment_flush_threshold() {
        snowseek::index::PersistentBuildOptions options;
        options.segment_flush_threshold_bytes = 0;
        snowseek::test::require_throws<std::invalid_argument>(
                [&options] {
                        static_cast<void>(snowseek::index::IndexBuilder(options));
                },
                "a zero flush threshold should be rejected");
}

/** @brief Verifies multi-Segment output matches a single-batch build. */
void merges_flushed_segments_byte_for_byte() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto single_directory = temporary.path() / "single";
        const auto merged_directory = temporary.path() / "merged";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "alpha shared alpha");
        write_file(source / "b.txt", "beta shared");
        write_file(source / "c.txt", "gamma shared gamma");

        snowseek::index::PersistentBuildOptions single_options;
        single_options.segment_flush_threshold_bytes =
                std::numeric_limits<std::uint64_t>::max();
        const auto single =
                snowseek::index::IndexBuilder(single_options)
                        .build(source, single_directory);

        snowseek::index::PersistentBuildOptions merged_options;
        merged_options.segment_flush_threshold_bytes = 1;
        const auto merged =
                snowseek::index::IndexBuilder(merged_options)
                        .build(source, merged_directory);

        snowseek::test::require_equal(
                single.temporary_segment_count, std::uint64_t{1},
                "an unlimited threshold should create one temporary Segment");
        snowseek::test::require_equal(
                merged.temporary_segment_count, std::uint64_t{3},
                "a one-byte threshold should flush every document");
        snowseek::test::require_equal(
                read_file(single.index_file), read_file(merged.index_file),
                "K-way merge should reproduce single-batch v1 bytes");
        const auto loaded =
                snowseek::storage::read_index_file(merged.index_file);
        const auto *shared = loaded.index.find("shared");
        snowseek::test::require(shared != nullptr && shared->size() == 3,
                                "a shared term should merge all documents");
        snowseek::test::require_equal(
                (*shared)[2].document_id,
                snowseek::document::DocumentId{2},
                "later Segment document IDs should be remapped globally");

        std::size_t published_entries = 0;
        for (const auto &entry :
             std::filesystem::directory_iterator(merged_directory)) {
                static_cast<void>(entry);
                ++published_entries;
        }
        snowseek::test::require_equal(
                published_entries, std::size_t{1},
                "successful publication should clean every workspace file");
}

/** @brief Verifies empty and one-document persistent builds. */
void handles_empty_and_oversized_batches() {
        const TemporaryDirectory temporary;
        const auto empty_source = temporary.path() / "empty-source";
        const auto one_source = temporary.path() / "one-source";
        std::filesystem::create_directory(empty_source);
        std::filesystem::create_directory(one_source);
        write_file(one_source / "large.txt", "one two three four five");

        snowseek::index::PersistentBuildOptions options;
        options.segment_flush_threshold_bytes = 1;
        const auto empty = snowseek::index::IndexBuilder(options).build(
                empty_source, temporary.path() / "empty-index");
        const auto one = snowseek::index::IndexBuilder(options).build(
                one_source, temporary.path() / "one-index");

        snowseek::test::require_equal(
                empty.temporary_segment_count, std::uint64_t{0},
                "an empty corpus should not create temporary Segments");
        snowseek::test::require_equal(
                snowseek::storage::validate_index_file(empty.index_file)
                        .document_count,
                std::uint64_t{0}, "an empty corpus should publish a valid index");
        snowseek::test::require_equal(
                one.temporary_segment_count, std::uint64_t{1},
                "one oversized document should flush after its complete commit");
        snowseek::test::require_equal(
                snowseek::storage::validate_index_file(one.index_file)
                        .document_count,
                std::uint64_t{1},
                "one oversized document should remain queryable");
}

/** @brief Verifies failed publication preserves the target and cleans work. */
void cleans_workspace_after_publication_failure() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        const auto published = destination /
                               snowseek::storage::kSegmentFileName;
        std::filesystem::create_directory(source);
        std::filesystem::create_directory(destination);
        std::filesystem::create_directory(published);
        write_file(source / "a.txt", "alpha");
        write_file(published / "sentinel", "old target");

        snowseek::test::require_throws<std::runtime_error>(
                [&source, &destination] {
                        static_cast<void>(snowseek::index::IndexBuilder{}.build(
                                source, destination));
                },
                "an unreplaceable published target should fail");
        snowseek::test::require(
                std::filesystem::exists(published / "sentinel"),
                "a failed publish should preserve the existing target");
        std::size_t entries = 0;
        for (const auto &entry :
             std::filesystem::directory_iterator(destination)) {
                static_cast<void>(entry);
                ++entries;
        }
        snowseek::test::require_equal(
                entries, std::size_t{1},
                "a failed publish should remove its private workspace");
}

} // namespace

/** @brief Runs the in-memory-index-builder integration-test suite. */
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
                {"reports deterministic memory estimates",
                 reports_deterministic_memory_estimates},
                {"reports zero memory for an empty corpus",
                 reports_zero_memory_for_an_empty_corpus},
                {"counts failed document buffers",
                 counts_failed_document_buffers},
                {"rejects zero Segment flush threshold",
                 rejects_zero_segment_flush_threshold},
                {"merges flushed Segments byte for byte",
                 merges_flushed_segments_byte_for_byte},
                {"handles empty and oversized batches",
                 handles_empty_and_oversized_batches},
                {"cleans workspace after publication failure",
                 cleans_workspace_after_publication_failure},
        });
}
