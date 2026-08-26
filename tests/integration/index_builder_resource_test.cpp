#include "index/index_builder.hpp"
#include "storage/index_file.hpp"

#include "support/index_builder_fixture.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using snowseek::test::read_file;
using snowseek::test::TemporaryDirectory;
using snowseek::test::write_file;

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
                "parallel K-way merge should reproduce serial v2 bytes");
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

/** @brief Verifies update delta bytes are independent of worker scheduling. */
void updates_deterministically_across_thread_counts() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "old alpha");
        const std::array<std::size_t, 3> thread_counts{1, 2, 4};
        std::array<std::filesystem::path, 3> destinations;
        for (std::size_t index = 0; index < destinations.size(); ++index) {
                destinations[index] =
                        temporary.path() / ("index-" + std::to_string(index));
                static_cast<void>(snowseek::index::IndexBuilder{}.build(
                        source, destinations[index]));
        }
        write_file(source / "a.txt", "new alpha beta");
        write_file(source / "b.txt", "beta gamma");

        std::array<std::string, 3> delta_bytes;
        for (std::size_t index = 0; index < destinations.size(); ++index) {
                auto options = snowseek::index::PersistentBuildOptions{};
                options.worker_thread_count = thread_counts[index];
                const auto result =
                        snowseek::index::IndexBuilder(options).update(
                                source, destinations[index]);
                delta_bytes[index] = read_file(result.index_file);
        }
        snowseek::test::require(
                delta_bytes[0] == delta_bytes[1] &&
                        delta_bytes[1] == delta_bytes[2],
                "threads 1, 2, and 4 should emit identical update Segments");
}

} // namespace

/** @brief Runs resource-budget and deterministic-output integration tests. */
int main() {
        return snowseek::test::run({
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
                {"updates deterministically across thread counts",
                 updates_deterministically_across_thread_counts},
        });
}
