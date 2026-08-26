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

/** @brief Verifies persistent builder resource limits. */
void validates_persistent_resource_options() {
        const auto defaults = snowseek::index::PersistentBuildOptions{};
        const auto balanced = snowseek::index::persistent_build_options(
                snowseek::index::ResourceProfile::balanced);
        snowseek::test::require_equal(
                defaults.memory_budget_bytes, balanced.memory_budget_bytes,
                "default options should use the Balanced memory budget");
        const auto minimal = snowseek::index::persistent_build_options(
                snowseek::index::ResourceProfile::minimal);
        snowseek::test::require_equal(
                minimal.memory_budget_bytes, 128ULL * 1024ULL * 1024ULL,
                "Minimal should use a 128 MiB memory budget");
        snowseek::test::require_equal(minimal.worker_thread_count,
                                      std::size_t{1},
                                      "Minimal should be single-threaded");
        snowseek::test::require(!minimal.in_memory_options.store_positions,
                                "Minimal should disable positions");
        const auto performance = snowseek::index::persistent_build_options(
                snowseek::index::ResourceProfile::performance);
        snowseek::test::require(
                performance.worker_thread_count >= 1 &&
                        performance.merge_fan_in == 32,
                "Performance should use detected threads and fan-in 32");

        snowseek::index::PersistentBuildOptions options;
        options.segment_flush_threshold_bytes = 0;
        snowseek::test::require_throws<std::invalid_argument>(
                [&options] {
                        static_cast<void>(
                                snowseek::index::IndexBuilder(options));
                },
                "a zero flush threshold should be rejected");

        options = {};
        options.temporary_space_budget_bytes = 0;
        snowseek::test::require_throws<std::invalid_argument>(
                [&options] {
                        static_cast<void>(
                                snowseek::index::IndexBuilder(options));
                },
                "a zero temporary-space budget should be rejected");

        for (const std::size_t fan_in : {std::size_t{0}, std::size_t{1}}) {
                options = {};
                options.merge_fan_in = fan_in;
                snowseek::test::require_throws<std::invalid_argument>(
                        [&options] {
                                static_cast<void>(
                                        snowseek::index::IndexBuilder(options));
                        },
                        "a fan-in below two should be rejected");
        }

        options.merge_fan_in = 2;
        static_cast<void>(snowseek::index::IndexBuilder(options));

        options = {};
        options.memory_budget_bytes = 0;
        snowseek::test::require_throws<std::invalid_argument>(
                [&options] {
                        static_cast<void>(
                                snowseek::index::IndexBuilder(options));
                },
                "a zero memory budget should be rejected");
        options = {};
        options.worker_thread_count = 0;
        snowseek::test::require_throws<std::invalid_argument>(
                [&options] {
                        static_cast<void>(
                                snowseek::index::IndexBuilder(options));
                },
                "a zero worker count should be rejected");
}

/** @brief Verifies bounded multi-level output matches a single-batch build. */
void merges_multiple_levels_byte_for_byte() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto single_directory = temporary.path() / "single";
        const auto merged_directory = temporary.path() / "merged";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "alpha shared alpha");
        write_file(source / "b.txt", "beta shared");
        write_file(source / "c.txt", "gamma shared gamma");
        write_file(source / "d.txt", "delta shared");
        write_file(source / "e.txt", "epsilon shared epsilon");

        snowseek::index::PersistentBuildOptions single_options;
        single_options.segment_flush_threshold_bytes =
                std::numeric_limits<std::uint64_t>::max();
        single_options.worker_thread_count = 1;
        const auto single = snowseek::index::IndexBuilder(single_options)
                                    .build(source, single_directory);

        snowseek::index::PersistentBuildOptions merged_options;
        merged_options.segment_flush_threshold_bytes = 1;
        merged_options.merge_fan_in = 2;
        merged_options.worker_thread_count = 4;
        const auto merged = snowseek::index::IndexBuilder(merged_options)
                                    .build(source, merged_directory);

        snowseek::test::require_equal(
                single.temporary_segment_count, std::uint64_t{1},
                "an unlimited threshold should create one temporary Segment");
        snowseek::test::require_equal(
                merged.temporary_segment_count, std::uint64_t{5},
                "a one-byte threshold should flush every document");
        snowseek::test::require_equal(
                merged.merge_pass_count, std::uint64_t{3},
                "five Segments with fan-in two should require three levels");
        snowseek::test::require_equal(
                read_file(single.index_file), read_file(merged.index_file),
                "parallel K-way merge should reproduce serial v1 bytes");
        snowseek::test::require(
                merged.memory_peak_bytes > 0 &&
                        merged.memory_peak_bytes <=
                                merged_options.memory_budget_bytes,
                "parallel logical memory should remain within budget");
        const auto loaded =
                snowseek::storage::read_index_file(merged.index_file);
        const auto *shared = loaded.index.find("shared");
        snowseek::test::require(shared != nullptr && shared->size() == 5,
                                "a shared term should merge all documents");
        snowseek::test::require_equal(
                (*shared)[4].document_id, snowseek::document::DocumentId{4},
                "later Segment document IDs should be remapped globally");

        std::size_t published_entries = 0;
        for (const auto &entry :
             std::filesystem::directory_iterator(merged_directory)) {
                static_cast<void>(entry);
                ++published_entries;
        }
        snowseek::test::require_equal(
                published_entries, std::size_t{2},
                "publication should retain only MANIFEST and its Segment");
}

/** @brief Verifies logical memory failures preserve published output. */
void enforces_logical_memory_budget() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "alpha beta gamma");

        auto allowed_options = snowseek::index::PersistentBuildOptions{};
        allowed_options.memory_budget_bytes = 1ULL << 20U;
        const auto allowed = snowseek::index::IndexBuilder(allowed_options)
                                     .build(source, destination);
        snowseek::test::require(
                allowed.memory_peak_bytes > 0 &&
                        allowed.memory_peak_bytes <= (1ULL << 20U),
                "a successful build should report an in-budget memory peak");
        const auto published_before = read_file(allowed.index_file);

        auto denied_options = snowseek::index::PersistentBuildOptions{};
        denied_options.memory_budget_bytes = 1;
        bool diagnosed = false;
        try {
                static_cast<void>(snowseek::index::IndexBuilder(denied_options)
                                          .build(source, destination));
        } catch (const std::runtime_error &error) {
                diagnosed = std::string(error.what())
                                    .find("memory limit exceeded") !=
                            std::string::npos;
        }
        snowseek::test::require(diagnosed,
                                "an undersized memory budget should fail");
        snowseek::test::require_equal(
                read_file(allowed.index_file), published_before,
                "a memory failure should preserve the published Segment");
        snowseek::test::require_equal(
                static_cast<std::size_t>(std::distance(
                        std::filesystem::directory_iterator(destination),
                        std::filesystem::directory_iterator{})),
                std::size_t{2},
                "a memory failure should retain only the published generation");
}

/** @brief Verifies positionless multi-level output remains deterministic. */
void merges_positionless_segments() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        std::filesystem::create_directory(source);
        for (const auto name : {"a.txt", "b.txt", "c.txt", "d.txt", "e.txt"}) {
                write_file(source / name, "alpha shared alpha");
        }
        auto single_options = snowseek::index::persistent_build_options(
                snowseek::index::ResourceProfile::minimal);
        single_options.segment_flush_threshold_bytes =
                std::numeric_limits<std::uint64_t>::max();
        const auto single = snowseek::index::IndexBuilder(single_options)
                                    .build(source, temporary.path() / "single");
        auto merged_options = single_options;
        merged_options.segment_flush_threshold_bytes = 1;
        merged_options.merge_fan_in = 2;
        const auto merged = snowseek::index::IndexBuilder(merged_options)
                                    .build(source, temporary.path() / "merged");
        snowseek::test::require_equal(
                read_file(single.index_file), read_file(merged.index_file),
                "positionless multi-level bytes should match one batch");
        const auto loaded =
                snowseek::storage::read_index_file(merged.index_file);
        const auto *alpha = loaded.index.find("alpha");
        snowseek::test::require(!loaded.index.stores_positions() &&
                                        loaded.stats.position_count == 0 &&
                                        alpha != nullptr &&
                                        (*alpha)[0].term_frequency() == 2 &&
                                        (*alpha)[0].positions.empty(),
                                "positionless indexes should retain frequency "
                                "without positions");
}

/** @brief Verifies hard temporary-space limits preserve published output. */
void enforces_temporary_space_budget() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "alpha beta gamma");

        snowseek::index::PersistentBuildOptions allowed_options;
        allowed_options.temporary_space_budget_bytes = 1ULL << 20U;
        const auto allowed = snowseek::index::IndexBuilder(allowed_options)
                                     .build(source, destination);
        snowseek::test::require(
                allowed.temporary_peak_bytes > 0 &&
                        allowed.temporary_peak_bytes <= (1ULL << 20U),
                "a successful build should report an in-budget peak");
        const auto published_before = read_file(allowed.index_file);

        snowseek::index::PersistentBuildOptions denied_options;
        denied_options.temporary_space_budget_bytes = 1;
        bool diagnosed = false;
        try {
                static_cast<void>(snowseek::index::IndexBuilder(denied_options)
                                          .build(source, destination));
        } catch (const std::runtime_error &error) {
                diagnosed = std::string(error.what())
                                    .find("temporary space budget exceeded") !=
                            std::string::npos;
        }
        snowseek::test::require(diagnosed,
                                "an undersized budget should report its limit");
        snowseek::test::require_equal(
                read_file(allowed.index_file), published_before,
                "a budget failure should preserve the published Segment");

        std::size_t entries = 0;
        for (const auto &entry :
             std::filesystem::directory_iterator(destination)) {
                static_cast<void>(entry);
                ++entries;
        }
        snowseek::test::require_equal(
                entries, std::size_t{2},
                "a budget failure should retain only the published generation");
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
                std::uint64_t{0},
                "an empty corpus should publish a valid index");
        snowseek::test::require_equal(one.temporary_segment_count,
                                      std::uint64_t{1},
                                      "one oversized document should flush "
                                      "after its complete commit");
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
        const auto published =
                destination / snowseek::storage::kSegmentFileName;
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
                {"validates persistent resource options",
                 validates_persistent_resource_options},
                {"merges multiple levels byte for byte",
                 merges_multiple_levels_byte_for_byte},
                {"enforces temporary space budget",
                 enforces_temporary_space_budget},
                {"enforces logical memory budget",
                 enforces_logical_memory_budget},
                {"merges positionless Segments", merges_positionless_segments},
                {"handles empty and oversized batches",
                 handles_empty_and_oversized_batches},
                {"cleans workspace after publication failure",
                 cleans_workspace_after_publication_failure},
        });
}
