#include "snowseek/filesystem/scanner.hpp"
#include "snowseek/index/index_builder.hpp"

#include "snowseek/common/checked_arithmetic.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <sys/resource.h>

namespace {

struct BenchmarkOptions {
        std::uint64_t files = 1024;
        std::uint64_t bytes_per_file = 64U * 1024U;
        std::uint64_t vocabulary = 4096;
};

class TemporaryDirectory {
      public:
        /** @brief Creates an isolated benchmark workspace below the system temp
         * directory.
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

        /** @brief Removes generated corpus and index files. */
        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        /** @brief Returns the benchmark workspace path. */
        [[nodiscard]] const std::filesystem::path &path() const noexcept {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

/**
 * @brief Parses one strictly positive decimal benchmark option.
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
 * @brief Parses benchmark size and vocabulary options.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Validated benchmark options.
 * @throws std::invalid_argument If an option is unknown, missing, or repeated.
 * @throws std::overflow_error If the requested corpus size overflows.
 */
[[nodiscard]] BenchmarkOptions parse_options(int argc, char *argv[]) {
        BenchmarkOptions options;
        bool has_files = false;
        bool has_bytes = false;
        bool has_vocabulary = false;
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
                } else if (option == "--files" ||
                           option == "--bytes-per-file" ||
                           option == "--vocabulary") {
                        throw std::invalid_argument(std::string(option) +
                                                    " may appear only once");
                } else {
                        throw std::invalid_argument("unknown option: " +
                                                    std::string(option));
                }
        }
        static_cast<void>(snowseek::common::detail::checked_multiply(
                options.files, options.bytes_per_file, "source bytes"));
        if (options.files > std::numeric_limits<std::size_t>::max() ||
            options.bytes_per_file > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                        "benchmark dimensions exceed size_t");
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
 * @brief Reads Linux process peak resident memory from getrusage.
 * @return Peak RSS in bytes.
 * @throws std::system_error If getrusage fails.
 * @throws std::overflow_error If KiB cannot be represented as bytes.
 */
[[nodiscard]] std::uint64_t peak_rss_bytes() {
        struct rusage usage {};
        if (::getrusage(RUSAGE_SELF, &usage) != 0) {
                const int error_number = errno;
                throw std::system_error(error_number, std::generic_category(),
                                        "getrusage failed");
        }
        if (usage.ru_maxrss < 0) {
                throw std::runtime_error("getrusage returned negative RSS");
        }
        return snowseek::common::detail::checked_multiply(
                static_cast<std::uint64_t>(usage.ru_maxrss), 1024,
                "peak RSS bytes");
}

/**
 * @brief Generates a corpus, builds its persistent index, and prints metrics.
 * @param options Validated benchmark dimensions.
 * @return Zero after a successful measurement.
 */
int run(const BenchmarkOptions &options) {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";

        generate_corpus(source, options);
        const auto rss_before = peak_rss_bytes();
        const auto started_at = std::chrono::steady_clock::now();
        const auto result =
                snowseek::index::IndexBuilder{}.build(source, destination);
        const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started_at);
        const auto rss_peak = peak_rss_bytes();
        const auto source_bytes = snowseek::common::detail::checked_multiply(
                options.files, options.bytes_per_file, "source bytes");
        const auto index_bytes = std::filesystem::file_size(result.index_file);
        const double elapsed_seconds =
                static_cast<double>(elapsed.count()) / 1'000'000.0;
        const double throughput = elapsed_seconds == 0.0
                                          ? 0.0
                                          : static_cast<double>(source_bytes) /
                                                    (1024.0 * 1024.0) /
                                                    elapsed_seconds;

        std::cout << "files=" << options.files << '\n'
                  << "bytes_per_file=" << options.bytes_per_file << '\n'
                  << "vocabulary=" << options.vocabulary << '\n'
                  << "source_bytes=" << source_bytes << '\n'
                  << "tokens=" << result.stats.token_count << '\n'
                  << "elapsed_us=" << elapsed.count() << '\n'
                  << "throughput_mib_per_second=" << std::fixed
                  << std::setprecision(3) << throughput << '\n'
                  << "index_bytes=" << index_bytes << '\n'
                  << "segment_flush_threshold_bytes="
                  << snowseek::index::kDefaultSegmentFlushThresholdBytes << '\n'
                  << "temporary_segment_count="
                  << result.temporary_segment_count << '\n'
                  << "merge_fan_in=" << snowseek::index::kDefaultMergeFanIn
                  << '\n'
                  << "merge_pass_count=" << result.merge_pass_count << '\n'
                  << "temporary_peak_bytes=" << result.temporary_peak_bytes
                  << '\n'
                  << "memory_metadata_bytes="
                  << result.stats.memory.metadata_bytes << '\n'
                  << "memory_reader_peak_bytes="
                  << result.stats.memory.reader_peak_bytes << '\n'
                  << "memory_token_peak_bytes="
                  << result.stats.memory.token_peak_bytes << '\n'
                  << "memory_dictionary_bytes="
                  << result.stats.memory.dictionary_bytes << '\n'
                  << "memory_posting_bytes="
                  << result.stats.memory.posting_bytes << '\n'
                  << "memory_estimated_peak_bytes="
                  << result.stats.memory.estimated_peak_bytes << '\n'
                  << "rss_before_build_bytes=" << rss_before << '\n'
                  << "rss_peak_bytes=" << rss_peak << '\n'
                  << "rss_increment_bytes="
                  << (rss_peak > rss_before ? rss_peak - rss_before : 0)
                  << '\n';
        return 0;
}

} // namespace

/**
 * @brief Runs the configurable persistent index-build benchmark.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Zero on success or one for invalid input and build failures.
 */
int main(int argc, char *argv[]) {
        try {
                return run(parse_options(argc, argv));
        } catch (const std::exception &error) {
                std::cerr << "snowseek benchmark: " << error.what() << '\n';
                return 1;
        }
}
