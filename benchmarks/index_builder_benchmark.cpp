/**
 * @file index_builder_benchmark.cpp
 * @brief Measures rebuild, update, and compaction under cold and hot file
 * caches.
 */

#include "filesystem/scanner.hpp"
#include "index/index_builder.hpp"
#include "storage/index_directory_internal.hpp"
#include "storage/index_file.hpp"

#include "common/checked_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

namespace {

using snowseek::common::detail::checked_add;
using snowseek::common::detail::checked_multiply;

enum class CacheMode {
        cold,
        hot,
};

struct BenchmarkOptions {
        std::uint64_t files{1024}; ///< Number of source files to generate.
        std::uint64_t bytes_per_file{64U * 1024U}; ///< Exact source file size.
        std::uint64_t vocabulary{4096}; ///< Distinct generated terms.
        std::uint64_t samples{10}; ///< Samples per operation and cache mode.
        std::uint64_t changed_files{1}; ///< Files modified before update.
        snowseek::index::ResourceProfile profile{
                snowseek::index::ResourceProfile::balanced}; ///< Build policy.
};

struct Measurement {
        std::uint64_t elapsed_us{}; ///< Timed operation latency.
        std::uint64_t kernel_write_bytes{}; ///< Linux write_bytes delta.
        std::uint64_t logical_input_bytes{}; ///< Throughput and amplification denominator.
        std::uint64_t active_bytes_before{}; ///< Active Segment bytes before the operation.
        std::uint64_t active_bytes_after{}; ///< Active Segment bytes after the operation.
        std::uint64_t directory_bytes_after{}; ///< Regular index-directory bytes afterward.
        std::uint64_t memory_peak_bytes{}; ///< Operation logical-memory peak.
        std::uint64_t temporary_peak_bytes{}; ///< Operation logical temporary peak.
};

struct ScenarioSummary {
        std::uint64_t samples{}; ///< Number of measurements summarized.
        std::uint64_t logical_input_bytes_per_sample{}; ///< Stable per-sample denominator.
        std::uint64_t logical_input_bytes_total{}; ///< Denominator summed across samples.
        std::uint64_t active_bytes_before{}; ///< Stable pre-operation Segment size.
        std::uint64_t active_bytes_after{}; ///< Stable post-operation Segment size.
        std::uint64_t directory_bytes_after{}; ///< Stable post-operation directory size.
        std::uint64_t elapsed_us_total{}; ///< Sum used for aggregate throughput.
        std::uint64_t elapsed_us_p50{}; ///< Nearest-rank median latency.
        std::uint64_t elapsed_us_p95{}; ///< Nearest-rank 95th percentile latency.
        std::uint64_t elapsed_us_p99{}; ///< Nearest-rank 99th percentile latency.
        std::uint64_t kernel_write_bytes_total{}; ///< Measured write_bytes sum.
        std::uint64_t memory_peak_bytes_max{}; ///< Largest logical-memory peak.
        std::uint64_t temporary_peak_bytes_max{}; ///< Largest temporary-space peak.
        double throughput_mib_per_second{}; ///< Aggregate logical throughput.
        double write_amplification{}; ///< Kernel writes divided by logical input.
};

/** @brief Owns one isolated benchmark tree below the system temp directory. */
class TemporaryDirectory {
      public:
        /**
         * @brief Creates a unique benchmark root.
         * @throws std::system_error If Linux cannot create the directory.
         */
        TemporaryDirectory() {
                auto pattern = (std::filesystem::temp_directory_path() /
                                "snowseek-index-benchmark-XXXXXX")
                                       .string();
                if (::mkdtemp(pattern.data()) == nullptr) {
                        throw std::system_error(errno, std::generic_category(),
                                                "mkdtemp failed");
                }
                path_ = std::move(pattern);
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;
        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        /** @brief Removes generated corpora, seeds, and sample indexes. */
        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        /** @brief Returns the benchmark root path. */
        [[nodiscard]] const std::filesystem::path &path() const noexcept {
                return path_;
        }

      private:
        std::filesystem::path path_; ///< Generated benchmark root.
};

/**
 * @brief Throws a path-aware system error using the current errno.
 * @param operation Failed operation named in the diagnostic.
 * @param path Related filesystem path.
 * @throws std::system_error Always.
 */
[[noreturn]] void throw_errno(std::string_view operation,
                              const std::filesystem::path &path) {
        throw std::system_error(errno, std::generic_category(),
                                std::string(operation) + ": " +
                                        path.string());
}

/**
 * @brief Parses one positive decimal benchmark option.
 * @param text Command-line value to parse.
 * @param option Option name used in diagnostics.
 * @return Parsed positive value.
 * @throws std::invalid_argument If text is empty, malformed, or zero.
 */
[[nodiscard]] std::uint64_t parse_positive(std::string_view text,
                                           std::string_view option) {
        std::uint64_t value = 0;
        const auto result =
                std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || result.ec != std::errc{} ||
            result.ptr != text.data() + text.size() || value == 0) {
                throw std::invalid_argument(std::string(option) +
                                            " requires a positive integer");
        }
        return value;
}

/**
 * @brief Parses an exact benchmark resource profile.
 * @param text Lowercase profile name.
 * @return Corresponding resource policy.
 * @throws std::invalid_argument If the profile is unsupported.
 */
[[nodiscard]] snowseek::index::ResourceProfile
parse_profile(std::string_view text) {
        if (text == "minimal") {
                return snowseek::index::ResourceProfile::minimal;
        }
        if (text == "balanced") {
                return snowseek::index::ResourceProfile::balanced;
        }
        if (text == "performance") {
                return snowseek::index::ResourceProfile::performance;
        }
        throw std::invalid_argument(
                "--profile requires minimal, balanced, or performance");
}

/**
 * @brief Returns the stable output spelling for a resource profile.
 * @param profile Resource policy to serialize.
 * @return Lowercase profile name.
 */
[[nodiscard]] std::string_view
profile_name(snowseek::index::ResourceProfile profile) noexcept {
        switch (profile) {
        case snowseek::index::ResourceProfile::minimal:
                return "minimal";
        case snowseek::index::ResourceProfile::balanced:
                return "balanced";
        case snowseek::index::ResourceProfile::performance:
                return "performance";
        }
        return "balanced";
}

/**
 * @brief Parses benchmark dimensions, profile, and sampling controls.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Validated benchmark options.
 * @throws std::invalid_argument If an option is unknown, repeated, or invalid.
 * @throws std::overflow_error If the requested corpus size overflows.
 */
[[nodiscard]] BenchmarkOptions parse_options(int argc, char *argv[]) {
        BenchmarkOptions options;
        bool has_files = false;
        bool has_bytes = false;
        bool has_vocabulary = false;
        bool has_samples = false;
        bool has_changed_files = false;
        bool has_profile = false;
        for (int index = 1; index < argc; ++index) {
                const std::string_view option(argv[index]);
                if (index + 1 >= argc) {
                        throw std::invalid_argument(std::string(option) +
                                                    " requires a value");
                }
                if (option == "--files" && !has_files) {
                        options.files = parse_positive(argv[++index], option);
                        has_files = true;
                } else if (option == "--bytes-per-file" && !has_bytes) {
                        options.bytes_per_file =
                                parse_positive(argv[++index], option);
                        has_bytes = true;
                } else if (option == "--vocabulary" && !has_vocabulary) {
                        options.vocabulary =
                                parse_positive(argv[++index], option);
                        has_vocabulary = true;
                } else if (option == "--samples" && !has_samples) {
                        options.samples = parse_positive(argv[++index], option);
                        has_samples = true;
                } else if (option == "--changed-files" && !has_changed_files) {
                        options.changed_files =
                                parse_positive(argv[++index], option);
                        has_changed_files = true;
                } else if (option == "--profile" && !has_profile) {
                        options.profile = parse_profile(argv[++index]);
                        has_profile = true;
                } else if (option == "--files" ||
                           option == "--bytes-per-file" ||
                           option == "--vocabulary" ||
                           option == "--samples" ||
                           option == "--changed-files" ||
                           option == "--profile") {
                        throw std::invalid_argument(std::string(option) +
                                                    " may appear only once");
                } else {
                        throw std::invalid_argument("unknown option: " +
                                                    std::string(option));
                }
        }

        static_cast<void>(checked_multiply(options.files,
                                           options.bytes_per_file,
                                           "source bytes"));
        if (options.files > std::numeric_limits<std::size_t>::max() ||
            options.bytes_per_file >
                    std::numeric_limits<std::size_t>::max() ||
            options.samples > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                        "benchmark dimensions exceed size_t");
        }
        if (options.changed_files > options.files) {
                throw std::invalid_argument(
                        "--changed-files must not exceed --files");
        }
        if (options.bytes_per_file >
            snowseek::filesystem::ScanOptions{}.max_file_size) {
                throw std::invalid_argument(
                        "--bytes-per-file exceeds the default scanner limit");
        }
        if (options.bytes_per_file >
            static_cast<std::uint64_t>(
                    std::numeric_limits<std::streamsize>::max())) {
                throw std::invalid_argument(
                        "--bytes-per-file exceeds streamsize");
        }
        return options;
}

/**
 * @brief Builds one deterministic text document of an exact byte size.
 * @param file_index Zero-based document index influencing term order.
 * @param byte_count Exact output size.
 * @param vocabulary Number of distinct generated terms.
 * @return ASCII document padded with spaces without splitting a token.
 */
[[nodiscard]] std::string make_document(std::uint64_t file_index,
                                        std::size_t byte_count,
                                        std::uint64_t vocabulary) {
        std::string document;
        document.reserve(byte_count);
        std::uint64_t token_index = 0;
        while (document.size() < byte_count) {
                const auto term_id = (file_index + token_index) % vocabulary;
                const std::string token =
                        "term_" + std::to_string(term_id) + ' ';
                const auto remaining = byte_count - document.size();
                if (token.size() > remaining) {
                        document.append(remaining, ' ');
                        break;
                }
                document.append(token);
                ++token_index;
        }
        return document;
}

/**
 * @brief Writes the deterministic benchmark corpus.
 * @param source Destination directory created by this function.
 * @param options Corpus dimensions and vocabulary.
 * @throws std::runtime_error If a directory or file cannot be written.
 */
void generate_corpus(const std::filesystem::path &source,
                     const BenchmarkOptions &options) {
        std::filesystem::create_directory(source);
        for (std::uint64_t file_index = 0; file_index < options.files;
             ++file_index) {
                const auto contents = make_document(
                        file_index,
                        static_cast<std::size_t>(options.bytes_per_file),
                        options.vocabulary);
                std::ofstream output(source / ("document-" +
                                               std::to_string(file_index) +
                                               ".txt"),
                                     std::ios::binary);
                output.write(contents.data(),
                             static_cast<std::streamsize>(contents.size()));
                if (!output) {
                        throw std::runtime_error(
                                "failed to write benchmark document");
                }
        }
}

/**
 * @brief Changes a deterministic prefix of the corpus without changing sizes.
 * @param source Existing generated corpus directory.
 * @param options Corpus dimensions and number of files to mutate.
 * @throws std::runtime_error If any fixture cannot be rewritten.
 */
void mutate_documents(const std::filesystem::path &source,
                      const BenchmarkOptions &options) {
        for (std::uint64_t file_index = 0;
             file_index < options.changed_files; ++file_index) {
                auto contents = make_document(
                        file_index,
                        static_cast<std::size_t>(options.bytes_per_file),
                        options.vocabulary);
                contents.back() = contents.back() == '\n' ? ' ' : '\n';
                std::ofstream output(source / ("document-" +
                                               std::to_string(file_index) +
                                               ".txt"),
                                     std::ios::binary | std::ios::trunc);
                output.write(contents.data(),
                             static_cast<std::streamsize>(contents.size()));
                if (!output) {
                        throw std::runtime_error(
                                "failed to mutate benchmark document");
                }
        }
}

/**
 * @brief Returns sorted regular files below one or more roots.
 * @param roots Files or directories whose regular files are requested.
 * @return Deterministically ordered paths.
 * @throws std::filesystem::filesystem_error If traversal or inspection fails.
 */
[[nodiscard]] std::vector<std::filesystem::path> regular_files(
        const std::vector<std::filesystem::path> &roots) {
        std::vector<std::filesystem::path> files;
        for (const auto &root : roots) {
                if (std::filesystem::is_regular_file(root)) {
                        files.push_back(root);
                        continue;
                }
                for (const auto &entry :
                     std::filesystem::recursive_directory_iterator(root)) {
                        if (entry.is_regular_file()) {
                                files.push_back(entry.path());
                        }
                }
        }
        std::sort(files.begin(), files.end());
        return files;
}

/**
 * @brief Reads every file byte to populate the Linux page cache.
 * @param files Regular files to pre-read in order.
 * @throws std::runtime_error If any file cannot be read completely.
 */
void warm_files(const std::vector<std::filesystem::path> &files) {
        std::array<char, 64 * 1024> buffer{};
        for (const auto &path : files) {
                std::ifstream input(path, std::ios::binary);
                if (!input) {
                        throw std::runtime_error("failed to open for cache warm: " +
                                                 path.string());
                }
                while (input.read(buffer.data(), buffer.size())) {
                }
                if (!input.eof()) {
                        throw std::runtime_error("failed to warm file cache: " +
                                                 path.string());
                }
        }
}

/**
 * @brief Flushes dirty data and advises Linux to discard cached file pages.
 * @param files Regular files to evict in order.
 * @throws std::system_error If open, fsync, fadvise, or close fails.
 */
void cool_files(const std::vector<std::filesystem::path> &files) {
        for (const auto &path : files) {
                snowseek::storage::detail::UniqueFd fd(
                        ::open(path.c_str(), O_RDONLY | O_CLOEXEC));
                if (fd.get() == -1) {
                        throw_errno("failed to open for cache eviction", path);
                }
                if (::fsync(fd.get()) == -1) {
                        throw_errno("failed to fsync before cache eviction",
                                    path);
                }
                const int advice = ::posix_fadvise(
                        fd.get(), 0, 0, POSIX_FADV_DONTNEED);
                if (advice != 0) {
                        throw std::system_error(
                                advice, std::generic_category(),
                                "failed to evict file cache: " + path.string());
                }
                fd.close_checked(path);
        }
}

/**
 * @brief Establishes one advisory file-content cache state outside timing.
 * @param mode Cold eviction or hot sequential pre-read.
 * @param roots Source and index roots relevant to the operation.
 * @throws std::runtime_error If any relevant file cannot be prepared.
 */
void prepare_cache(CacheMode mode,
                   const std::vector<std::filesystem::path> &roots) {
        const auto files = regular_files(roots);
        if (mode == CacheMode::cold) {
                cool_files(files);
        } else {
                warm_files(files);
        }
}

/**
 * @brief Copies one immutable seed index into a fresh sample directory.
 * @param source Existing flat index directory.
 * @param destination New sample directory to create.
 * @throws std::runtime_error If an unexpected entry is encountered.
 * @throws std::filesystem::filesystem_error If copying fails.
 */
void clone_index(const std::filesystem::path &source,
                 const std::filesystem::path &destination) {
        std::filesystem::create_directory(destination);
        for (const auto &entry : std::filesystem::directory_iterator(source)) {
                if (!entry.is_regular_file()) {
                        throw std::runtime_error(
                                "benchmark seed contains a non-file entry: " +
                                entry.path().string());
                }
                std::filesystem::copy_file(
                        entry.path(), destination / entry.path().filename());
        }
}

/**
 * @brief Sums regular-file lengths below one directory.
 * @param directory Index directory to inspect recursively.
 * @return Total logical bytes including MANIFEST and Segments.
 * @throws std::overflow_error If the total exceeds uint64_t.
 * @throws std::filesystem::filesystem_error If traversal fails.
 */
[[nodiscard]] std::uint64_t
directory_bytes(const std::filesystem::path &directory) {
        std::uint64_t bytes = 0;
        for (const auto &entry :
             std::filesystem::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                        bytes = checked_add(bytes, entry.file_size(),
                                            "benchmark directory bytes");
                }
        }
        return bytes;
}

/**
 * @brief Reads the current Linux process write_bytes counter.
 * @return Bytes this process caused the storage layer to write.
 * @throws std::runtime_error If /proc/self/io is unavailable or malformed.
 */
[[nodiscard]] std::uint64_t process_write_bytes() {
        std::ifstream input("/proc/self/io");
        std::string key;
        std::uint64_t value = 0;
        while (input >> key >> value) {
                if (key == "write_bytes:") {
                        return value;
                }
        }
        throw std::runtime_error(
                "failed to read write_bytes from /proc/self/io");
}

/**
 * @brief Reads Linux process peak resident memory from getrusage.
 * @return Process-lifetime peak RSS in bytes.
 * @throws std::system_error If getrusage fails.
 * @throws std::overflow_error If KiB cannot be represented as bytes.
 */
[[nodiscard]] std::uint64_t peak_rss_bytes() {
        struct rusage usage {};
        if (::getrusage(RUSAGE_SELF, &usage) != 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "getrusage failed");
        }
        if (usage.ru_maxrss < 0) {
                throw std::runtime_error("getrusage returned negative RSS");
        }
        return checked_multiply(static_cast<std::uint64_t>(usage.ru_maxrss),
                                1024, "peak RSS bytes");
}

/**
 * @brief Times one publication operation after establishing cache state.
 * @tparam Operation Callable returning PersistentBuildResult.
 * @param mode Cache state established immediately before measurement.
 * @param cache_roots Files relevant to the timed operation.
 * @param index_directory Sample index whose sizes are recorded.
 * @param logical_input_bytes Throughput and amplification denominator.
 * @param active_bytes_before Active Segment bytes before the operation.
 * @param expected_revision Manifest generation required after publication.
 * @param expected_segments Active Segment count required after publication.
 * @param expect_compacted Required result.compacted value.
 * @param operation Timed publication callable.
 * @return One validated operation measurement.
 * @throws std::runtime_error If publication or counters violate invariants.
 */
template <typename Operation>
[[nodiscard]] Measurement measure_operation(
        CacheMode mode,
        const std::vector<std::filesystem::path> &cache_roots,
        const std::filesystem::path &index_directory,
        std::uint64_t logical_input_bytes,
        std::uint64_t active_bytes_before, std::uint64_t expected_revision,
        std::uint64_t expected_segments, bool expect_compacted,
        Operation &&operation) {
        prepare_cache(mode, cache_roots);
        const auto writes_before = process_write_bytes();
        const auto started_at = std::chrono::steady_clock::now();
        const auto result = operation();
        const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started_at);
        const auto writes_after = process_write_bytes();
        if (!result.published || result.compacted != expect_compacted ||
            result.manifest_generation != expected_revision ||
            result.active_segment_count != expected_segments) {
                throw std::runtime_error(
                        "benchmark operation did not publish the expected state");
        }
        if (elapsed.count() < 0 || writes_after < writes_before) {
                throw std::runtime_error("benchmark counters moved backwards");
        }
        const auto active_bytes_after =
                snowseek::storage::validate_index_directory(index_directory)
                        .file_size;
        return Measurement{
                static_cast<std::uint64_t>(elapsed.count()),
                writes_after - writes_before,
                logical_input_bytes,
                active_bytes_before,
                active_bytes_after,
                directory_bytes(index_directory),
                result.memory_peak_bytes,
                result.temporary_peak_bytes,
        };
}

/**
 * @brief Selects one nearest-rank percentile from nonempty values.
 * @param values Unordered samples copied for sorting.
 * @param percentile Integer percentile in [1, 100].
 * @return Selected observed sample without interpolation.
 */
[[nodiscard]] std::uint64_t nearest_rank(
        std::vector<std::uint64_t> values, std::size_t percentile) {
        if (values.empty() || percentile == 0 || percentile > 100) {
                throw std::invalid_argument("invalid nearest-rank request");
        }
        std::sort(values.begin(), values.end());
        const auto whole = (values.size() / 100) * percentile;
        const auto remainder =
                ((values.size() % 100) * percentile + 99) / 100;
        return values[whole + remainder - 1];
}

/**
 * @brief Validates stable volume fields and aggregates one scenario.
 * @param measurements Nonempty operation samples.
 * @return Stable volumes, latency percentiles, throughput, and write ratio.
 */
[[nodiscard]] ScenarioSummary
summarize(const std::vector<Measurement> &measurements) {
        if (measurements.empty()) {
                throw std::invalid_argument(
                        "cannot summarize an empty benchmark scenario");
        }
        const auto &first = measurements.front();
        ScenarioSummary summary;
        summary.samples = measurements.size();
        summary.logical_input_bytes_per_sample = first.logical_input_bytes;
        summary.active_bytes_before = first.active_bytes_before;
        summary.active_bytes_after = first.active_bytes_after;
        summary.directory_bytes_after = first.directory_bytes_after;
        std::vector<std::uint64_t> latencies;
        latencies.reserve(measurements.size());
        for (const auto &measurement : measurements) {
                if (measurement.logical_input_bytes !=
                            summary.logical_input_bytes_per_sample ||
                    measurement.active_bytes_before !=
                            summary.active_bytes_before ||
                    measurement.active_bytes_after !=
                            summary.active_bytes_after ||
                    measurement.directory_bytes_after !=
                            summary.directory_bytes_after) {
                        throw std::runtime_error(
                                "benchmark scenario produced inconsistent volumes");
                }
                latencies.push_back(measurement.elapsed_us);
                summary.elapsed_us_total = checked_add(
                        summary.elapsed_us_total, measurement.elapsed_us,
                        "benchmark elapsed microseconds");
                summary.kernel_write_bytes_total = checked_add(
                        summary.kernel_write_bytes_total,
                        measurement.kernel_write_bytes,
                        "benchmark kernel write bytes");
                summary.logical_input_bytes_total = checked_add(
                        summary.logical_input_bytes_total,
                        measurement.logical_input_bytes,
                        "benchmark logical input bytes");
                summary.memory_peak_bytes_max = std::max(
                        summary.memory_peak_bytes_max,
                        measurement.memory_peak_bytes);
                summary.temporary_peak_bytes_max = std::max(
                        summary.temporary_peak_bytes_max,
                        measurement.temporary_peak_bytes);
        }
        summary.elapsed_us_p50 = nearest_rank(latencies, 50);
        summary.elapsed_us_p95 = nearest_rank(latencies, 95);
        summary.elapsed_us_p99 = nearest_rank(std::move(latencies), 99);
        if (summary.elapsed_us_total == 0 ||
            summary.logical_input_bytes_total == 0) {
                throw std::runtime_error(
                        "benchmark produced a zero timing denominator");
        }
        summary.throughput_mib_per_second =
                static_cast<double>(summary.logical_input_bytes_total) /
                (1024.0 * 1024.0) /
                (static_cast<double>(summary.elapsed_us_total) / 1'000'000.0);
        summary.write_amplification =
                static_cast<double>(summary.kernel_write_bytes_total) /
                static_cast<double>(summary.logical_input_bytes_total);
        return summary;
}

/**
 * @brief Removes one completed sample directory and reports cleanup failures.
 * @param directory Sample index owned by the benchmark root.
 * @throws std::runtime_error If recursive removal fails.
 */
void remove_sample(const std::filesystem::path &directory) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        if (error) {
                throw std::runtime_error("failed to remove benchmark sample: " +
                                         error.message());
        }
}

/**
 * @brief Returns a unique sample-directory name for one scenario.
 * @param root Benchmark root owning all samples.
 * @param operation Stable operation spelling.
 * @param mode Cold or hot cache state.
 * @param sample Zero-based sample number.
 * @return Path not used by another scenario.
 */
[[nodiscard]] std::filesystem::path sample_path(
        const std::filesystem::path &root, std::string_view operation,
        CacheMode mode, std::uint64_t sample) {
        const auto cache = mode == CacheMode::cold ? "cold" : "hot";
        return root / (std::string(operation) + '-' + cache + '-' +
                       std::to_string(sample));
}

/**
 * @brief Measures repeated complete rebuilds into empty directories.
 * @param root Benchmark root owning sample directories.
 * @param source Mutated corpus indexed by every sample.
 * @param builder Configured persistent builder.
 * @param options Sampling and corpus dimensions.
 * @param mode Cold or hot file-content cache.
 * @return Aggregated rebuild measurements.
 */
[[nodiscard]] ScenarioSummary benchmark_rebuild(
        const std::filesystem::path &root,
        const std::filesystem::path &source,
        const snowseek::index::IndexBuilder &builder,
        const BenchmarkOptions &options, CacheMode mode) {
        const auto source_bytes = checked_multiply(
                options.files, options.bytes_per_file, "source bytes");
        std::vector<Measurement> measurements;
        measurements.reserve(static_cast<std::size_t>(options.samples));
        for (std::uint64_t sample = 0; sample < options.samples; ++sample) {
                const auto index = sample_path(root, "rebuild", mode, sample);
                measurements.push_back(measure_operation(
                        mode, {source}, index, source_bytes, 0, 1, 1, false,
                        [&builder, &source, &index] {
                                return builder.build(source, index);
                        }));
                remove_sample(index);
        }
        return summarize(measurements);
}

/**
 * @brief Measures updates from copies of one immutable old-generation seed.
 * @param root Benchmark root owning sample directories.
 * @param source Mutated corpus synchronized by every sample.
 * @param seed Old-generation index copied before timing.
 * @param builder Configured persistent builder.
 * @param options Sampling and changed-file dimensions.
 * @param mode Cold or hot file-content cache.
 * @return Aggregated update measurements.
 */
[[nodiscard]] ScenarioSummary benchmark_update(
        const std::filesystem::path &root,
        const std::filesystem::path &source,
        const std::filesystem::path &seed,
        const snowseek::index::IndexBuilder &builder,
        const BenchmarkOptions &options, CacheMode mode) {
        const auto changed_bytes = checked_multiply(
                options.changed_files, options.bytes_per_file,
                "changed source bytes");
        std::vector<Measurement> measurements;
        measurements.reserve(static_cast<std::size_t>(options.samples));
        for (std::uint64_t sample = 0; sample < options.samples; ++sample) {
                const auto index = sample_path(root, "update", mode, sample);
                clone_index(seed, index);
                const auto before =
                        snowseek::storage::validate_index_directory(index)
                                .file_size;
                measurements.push_back(measure_operation(
                        mode, {source, index}, index, changed_bytes, before, 2,
                        2, false, [&builder, &source, &index] {
                                return builder.update(source, index);
                        }));
                remove_sample(index);
        }
        return summarize(measurements);
}

/**
 * @brief Measures compaction from copies of one immutable two-Segment seed.
 * @param root Benchmark root owning sample directories.
 * @param seed Updated index copied before timing.
 * @param builder Configured persistent builder.
 * @param options Sampling dimensions.
 * @param mode Cold or hot file-content cache.
 * @return Aggregated compaction measurements.
 */
[[nodiscard]] ScenarioSummary benchmark_compact(
        const std::filesystem::path &root,
        const std::filesystem::path &seed,
        const snowseek::index::IndexBuilder &builder,
        const BenchmarkOptions &options, CacheMode mode) {
        std::vector<Measurement> measurements;
        measurements.reserve(static_cast<std::size_t>(options.samples));
        for (std::uint64_t sample = 0; sample < options.samples; ++sample) {
                const auto index = sample_path(root, "compact", mode, sample);
                clone_index(seed, index);
                const auto before =
                        snowseek::storage::validate_index_directory(index)
                                .file_size;
                measurements.push_back(measure_operation(
                        mode, {index}, index, before, before, 3, 1, true,
                        [&builder, &index] { return builder.compact(index); }));
                remove_sample(index);
        }
        return summarize(measurements);
}

/**
 * @brief Prints one stable prefixed key-value scenario block.
 * @param prefix Operation and cache prefix such as rebuild.cold.
 * @param summary Aggregated metrics to serialize.
 */
void print_summary(std::string_view prefix,
                   const ScenarioSummary &summary) {
        std::cout << prefix << ".samples=" << summary.samples << '\n'
                  << prefix << ".logical_input_bytes_per_sample="
                  << summary.logical_input_bytes_per_sample << '\n'
                  << prefix << ".logical_input_bytes_total="
                  << summary.logical_input_bytes_total << '\n'
                  << prefix << ".active_segment_bytes_before="
                  << summary.active_bytes_before << '\n'
                  << prefix << ".active_segment_bytes_after="
                  << summary.active_bytes_after << '\n'
                  << prefix << ".directory_bytes_after="
                  << summary.directory_bytes_after << '\n'
                  << prefix << ".elapsed_us_total="
                  << summary.elapsed_us_total << '\n'
                  << prefix << ".elapsed_us_p50=" << summary.elapsed_us_p50
                  << '\n'
                  << prefix << ".elapsed_us_p95=" << summary.elapsed_us_p95
                  << '\n'
                  << prefix << ".elapsed_us_p99=" << summary.elapsed_us_p99
                  << '\n'
                  << prefix << ".throughput_mib_per_second=" << std::fixed
                  << std::setprecision(3)
                  << summary.throughput_mib_per_second << '\n'
                  << prefix << ".kernel_write_bytes_total="
                  << summary.kernel_write_bytes_total << '\n'
                  << prefix << ".write_amplification="
                  << summary.write_amplification << '\n'
                  << prefix << ".memory_peak_bytes_max="
                  << summary.memory_peak_bytes_max << '\n'
                  << prefix << ".temporary_peak_bytes_max="
                  << summary.temporary_peak_bytes_max << '\n';
}

/**
 * @brief Prepares immutable seeds, measures all six scenarios, and prints them.
 * @param options Validated corpus and sampling configuration.
 * @return Zero after successful measurement and serialization.
 * @throws std::runtime_error If setup or any scenario violates invariants.
 */
int run(const BenchmarkOptions &options) {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto old_seed = temporary.path() / "old-seed";
        const auto compact_seed = temporary.path() / "compact-seed";
        generate_corpus(source, options);
        const auto build_options =
                snowseek::index::persistent_build_options(options.profile);
        const snowseek::index::IndexBuilder builder(build_options);

        // Build the old generation once, then mutate the shared corpus.
        const auto seeded_build = builder.build(source, old_seed);
        if (!seeded_build.published || seeded_build.compacted ||
            seeded_build.manifest_generation != 1 ||
            seeded_build.active_segment_count != 1) {
                throw std::runtime_error(
                        "failed to prepare one-Segment update seed");
        }
        mutate_documents(source, options);

        // Create the immutable two-Segment state used by compaction samples.
        clone_index(old_seed, compact_seed);
        const auto seeded_update = builder.update(source, compact_seed);
        if (!seeded_update.published || seeded_update.compacted ||
            seeded_update.manifest_generation != 2 ||
            seeded_update.active_segment_count != 2) {
                throw std::runtime_error(
                        "failed to prepare two-Segment compaction seed");
        }

        const auto rebuild_cold = benchmark_rebuild(
                temporary.path(), source, builder, options, CacheMode::cold);
        const auto rebuild_hot = benchmark_rebuild(
                temporary.path(), source, builder, options, CacheMode::hot);
        const auto update_cold = benchmark_update(
                temporary.path(), source, old_seed, builder, options,
                CacheMode::cold);
        const auto update_hot = benchmark_update(
                temporary.path(), source, old_seed, builder, options,
                CacheMode::hot);
        const auto compact_cold = benchmark_compact(
                temporary.path(), compact_seed, builder, options,
                CacheMode::cold);
        const auto compact_hot = benchmark_compact(
                temporary.path(), compact_seed, builder, options,
                CacheMode::hot);

        std::cout << "files=" << options.files << '\n'
                  << "bytes_per_file=" << options.bytes_per_file << '\n'
                  << "vocabulary=" << options.vocabulary << '\n'
                  << "changed_files=" << options.changed_files << '\n'
                  << "samples_per_scenario=" << options.samples << '\n'
                  << "resource_profile=" << profile_name(options.profile)
                  << '\n'
                  << "cold_cache_method=fsync_posix_fadvise_dontneed\n"
                  << "hot_cache_method=sequential_preread\n";
        print_summary("rebuild.cold", rebuild_cold);
        print_summary("rebuild.hot", rebuild_hot);
        print_summary("update.cold", update_cold);
        print_summary("update.hot", update_hot);
        print_summary("compact.cold", compact_cold);
        print_summary("compact.hot", compact_hot);
        std::cout << "process_lifetime_rss_peak_bytes=" << peak_rss_bytes()
                  << '\n';
        return 0;
}

} // namespace

/**
 * @brief Runs the configurable Linux persistent-maintenance benchmark.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Zero on success or one for invalid input and measurement failures.
 */
int main(int argc, char *argv[]) {
        try {
                return run(parse_options(argc, argv));
        } catch (const std::exception &error) {
                std::cerr << "snowseek benchmark: " << error.what() << '\n';
                return 1;
        }
}
