#include "snowseek/document/text_reader.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

class TemporaryDirectory {
      public:
        TemporaryDirectory() {
                const auto seed = std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count();
                const auto base = std::filesystem::temp_directory_path();
                for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
                        path_ = base / ("snowseek-reader-test-" +
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
                throw std::runtime_error("failed to write reader test file");
        }
}

std::string read_all(const snowseek::document::TextReader &reader,
                     const std::filesystem::path &path,
                     snowseek::document::TextReadStats &stats,
                     std::size_t &consumer_calls) {
        std::string output;
        stats = reader.read(path,
                            [&output, &consumer_calls](std::string_view chunk) {
                                    output.append(chunk);
                                    ++consumer_calls;
                            });
        return output;
}

void rejects_invalid_configuration() {
        snowseek::document::TextReadOptions options;
        options.chunk_size = 0;
        snowseek::test::require_throws<std::invalid_argument>(
                [&options] {
                        const snowseek::document::TextReader reader(options);
                        static_cast<void>(reader);
                },
                "a zero-sized chunk should be rejected");
}

void reads_ascii_in_bounded_chunks() {
        const TemporaryDirectory temporary;
        const auto path = temporary.path() / "ascii.txt";
        write_file(path, "abcdefghij");

        snowseek::document::TextReadOptions options;
        options.chunk_size = 3;
        const snowseek::document::TextReader reader(options);
        snowseek::document::TextReadStats stats;
        std::size_t consumer_calls = 0;
        const auto output = read_all(reader, path, stats, consumer_calls);

        snowseek::test::require_equal(output, std::string("abcdefghij"),
                                      "ASCII content should be preserved");
        snowseek::test::require_equal(stats.bytes_read, std::uint64_t{10},
                                      "all source bytes should be read");
        snowseek::test::require_equal(stats.bytes_emitted, std::uint64_t{10},
                                      "all ASCII bytes should be emitted");
        snowseek::test::require_equal(
                stats.invalid_sequence_count, std::uint64_t{0},
                "valid ASCII should have no invalid sequences");
        snowseek::test::require_equal(
                consumer_calls, std::size_t{4},
                "the consumer should receive multiple bounded chunks");
}

void preserves_utf8_across_chunk_boundaries() {
        const TemporaryDirectory temporary;
        const auto path = temporary.path() / "utf8.txt";
        const std::string contents{"A\xe4\xb8\xad"
                                   "B",
                                   5};
        write_file(path, contents);

        snowseek::document::TextReadOptions options;
        options.chunk_size = 2;
        const snowseek::document::TextReader reader(options);
        snowseek::document::TextReadStats stats;
        std::size_t consumer_calls = 0;
        const auto output = read_all(reader, path, stats, consumer_calls);

        snowseek::test::require_equal(
                output, contents,
                "a UTF-8 character split across chunks should be preserved");
        snowseek::test::require_equal(
                stats.invalid_sequence_count, std::uint64_t{0},
                "split valid UTF-8 should not be reported as invalid");
}

void replaces_invalid_utf8() {
        const TemporaryDirectory temporary;
        const auto path = temporary.path() / "invalid.txt";
        std::string contents{"A"};
        contents.push_back(static_cast<char>(0xe2));
        contents.push_back('(');
        contents.push_back(static_cast<char>(0xa1));
        contents.push_back('B');
        write_file(path, contents);

        snowseek::document::TextReadOptions options;
        options.chunk_size = 2;
        const snowseek::document::TextReader reader(options);
        snowseek::document::TextReadStats stats;
        std::size_t consumer_calls = 0;
        const auto output = read_all(reader, path, stats, consumer_calls);
        const std::string expected{"A\xef\xbf\xbd(\xef\xbf\xbd"
                                   "B",
                                   9};

        snowseek::test::require_equal(
                output, expected,
                "invalid sequences should be replaced without losing ASCII");
        snowseek::test::require_equal(
                stats.invalid_sequence_count, std::uint64_t{2},
                "each invalid sequence should be counted");
        snowseek::test::require_equal(stats.bytes_read, std::uint64_t{5},
                                      "stats should count original bytes");
        snowseek::test::require_equal(stats.bytes_emitted, std::uint64_t{9},
                                      "stats should count replacement bytes");
}

void rejects_invalid_utf8_with_offset() {
        const TemporaryDirectory temporary;
        const auto path = temporary.path() / "strict.txt";
        std::string contents{"xy"};
        contents.push_back(static_cast<char>(0xe2));
        contents.push_back('(');
        write_file(path, contents);

        snowseek::document::TextReadOptions options;
        options.chunk_size = 3;
        options.invalid_utf8_policy =
                snowseek::document::InvalidUtf8Policy::reject;
        const snowseek::document::TextReader reader(options);

        try {
                static_cast<void>(reader.read(path, [](std::string_view) {}));
        } catch (const snowseek::document::InvalidUtf8Error &error) {
                snowseek::test::require_equal(
                        error.path(), path,
                        "strict errors should identify the source file");
                snowseek::test::require_equal(
                        error.byte_offset(), std::uint64_t{2},
                        "strict errors should report the original byte offset");
                return;
        }
        throw std::runtime_error("strict mode should reject invalid UTF-8");
}

void replaces_truncated_sequence_at_end_of_file() {
        const TemporaryDirectory temporary;
        const auto path = temporary.path() / "truncated.txt";
        std::string contents{"A"};
        contents.push_back(static_cast<char>(0xf0));
        contents.push_back(static_cast<char>(0x9f));
        write_file(path, contents);

        snowseek::document::TextReadOptions options;
        options.chunk_size = 2;
        const snowseek::document::TextReader reader(options);
        snowseek::document::TextReadStats stats;
        std::size_t consumer_calls = 0;
        const auto output = read_all(reader, path, stats, consumer_calls);
        const std::string expected{"A\xef\xbf\xbd", 4};

        snowseek::test::require_equal(
                output, expected,
                "a truncated final sequence should become one replacement");
        snowseek::test::require_equal(
                stats.invalid_sequence_count, std::uint64_t{1},
                "a truncated prefix should count as one invalid sequence");
}

void handles_empty_and_missing_files() {
        const TemporaryDirectory temporary;
        const auto empty = temporary.path() / "empty.txt";
        write_file(empty, "");

        const snowseek::document::TextReader reader;
        snowseek::document::TextReadStats stats;
        std::size_t consumer_calls = 0;
        const auto output = read_all(reader, empty, stats, consumer_calls);
        snowseek::test::require(output.empty(),
                                "an empty file should emit no content");
        snowseek::test::require_equal(stats.bytes_read, std::uint64_t{0},
                                      "an empty file should read zero bytes");
        snowseek::test::require_equal(
                consumer_calls, std::size_t{0},
                "an empty file should not invoke the consumer");

        snowseek::test::require_throws<std::runtime_error>(
                [&reader, &temporary] {
                        static_cast<void>(
                                reader.read(temporary.path() / "missing.txt",
                                            [](std::string_view) {}));
                },
                "a missing file should be rejected");
}

} // namespace

int main() {
        return snowseek::test::run({
                {"rejects invalid configuration",
                 rejects_invalid_configuration},
                {"reads ASCII in bounded chunks",
                 reads_ascii_in_bounded_chunks},
                {"preserves UTF-8 across chunk boundaries",
                 preserves_utf8_across_chunk_boundaries},
                {"replaces invalid UTF-8", replaces_invalid_utf8},
                {"rejects invalid UTF-8 with offset",
                 rejects_invalid_utf8_with_offset},
                {"replaces a truncated sequence at EOF",
                 replaces_truncated_sequence_at_end_of_file},
                {"handles empty and missing files",
                 handles_empty_and_missing_files},
        });
}
