#include "snowseek/index/in_memory_index_builder.hpp"

#include <cerrno>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace snowseek::index {
namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;

struct FileMetadata {
        std::uint64_t file_size{};
        std::int64_t modified_time_ns{};
};

/**
 * @brief Converts a POSIX modification timestamp to Unix Epoch nanoseconds.
 * @param status File status containing the timestamp to convert.
 * @return The signed nanosecond timestamp.
 * @throws std::runtime_error If the nanosecond component is invalid.
 * @throws std::overflow_error If the timestamp cannot fit in std::int64_t.
 */
[[nodiscard]] std::int64_t unix_time_nanoseconds(const struct stat &status) {
        const auto seconds = static_cast<std::int64_t>(status.st_mtim.tv_sec);
        const auto nanoseconds =
                static_cast<std::int64_t>(status.st_mtim.tv_nsec);

        if (nanoseconds < 0 || nanoseconds >= kNanosecondsPerSecond) {
                throw std::runtime_error(
                        "file modification time has invalid nanoseconds");
        }
        if (seconds > std::numeric_limits<std::int64_t>::max() /
                              kNanosecondsPerSecond ||
            seconds < std::numeric_limits<std::int64_t>::min() /
                              kNanosecondsPerSecond) {
                throw std::overflow_error(
                        "file modification time is outside int64_t");
        }
        return seconds * kNanosecondsPerSecond + nanoseconds;
}

/**
 * @brief Reads and validates the metadata required for an indexed document.
 * @param path Candidate file whose metadata is requested.
 * @return The regular-file size and modification timestamp.
 * @throws std::system_error If the operating system cannot read the metadata.
 * @throws std::runtime_error If the path is not a valid regular file.
 * @throws std::overflow_error If its modification timestamp is unsupported.
 */
[[nodiscard]] FileMetadata read_metadata(const std::filesystem::path &path) {
        struct stat status {};
        if (::stat(path.c_str(), &status) != 0) {
                const int error_number = errno;
                throw std::system_error(error_number, std::generic_category(),
                                        "failed to read file metadata: " +
                                                path.string());
        }
        if (!S_ISREG(status.st_mode)) {
                throw std::runtime_error("path is not a regular file: " +
                                         path.string());
        }
        if (status.st_size < 0) {
                throw std::runtime_error("file has a negative size: " +
                                         path.string());
        }

        return FileMetadata{static_cast<std::uint64_t>(status.st_size),
                            unix_time_nanoseconds(status)};
}

struct ParsedDocument {
        FileMetadata metadata;
        document::TextReadStats read_stats;
        std::vector<analysis::Token> tokens;
};

/**
 * @brief Adds two build counters without unsigned wraparound.
 * @param left Current counter value.
 * @param right Increment to apply.
 * @param field Counter name included in an overflow diagnostic.
 * @return The checked sum of both values.
 * @throws std::overflow_error If the addition exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t checked_sum(std::uint64_t left, std::uint64_t right,
                                        std::string_view field) {
        if (right > std::numeric_limits<std::uint64_t>::max() - left) {
                throw std::overflow_error("build statistic overflow: " +
                                          std::string(field));
        }
        return left + right;
}

/**
 * @brief Reads and tokenizes one file without mutating the build result.
 * @param path Source file to parse.
 * @param reader Configured reader used to stream validated UTF-8 chunks.
 * @param tokenizer_options Rules and limits applied during tokenization.
 * @return Metadata, read statistics, and the complete token sequence.
 * @throws std::runtime_error If metadata, reading, or UTF-8 validation fails.
 * @throws std::length_error If a token exceeds the configured limit.
 * @throws std::overflow_error If the token count exceeds std::uint32_t.
 */
[[nodiscard]] ParsedDocument
parse_document(const std::filesystem::path &path,
               const document::TextReader &reader,
               const analysis::TokenizerOptions &tokenizer_options) {
        ParsedDocument parsed;

        // Capture metadata before reading so a changed or invalid candidate is
        // rejected before any token work is retained.
        parsed.metadata = read_metadata(path);

        // Stream reader chunks through one tokenizer session so tokens may span
        // chunk boundaries while positions remain continuous.
        analysis::TokenizerSession tokenizer(tokenizer_options);
        const analysis::TokenConsumer token_consumer =
                [&parsed](analysis::Token token) {
                        parsed.tokens.push_back(std::move(token));
                };
        parsed.read_stats = reader.read(
                path, [&tokenizer, &token_consumer](std::string_view chunk) {
                        tokenizer.push(chunk, token_consumer);
                });
        tokenizer.finish(token_consumer);

        if (parsed.tokens.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(
                        "document token count exceeds uint32_t");
        }
        return parsed;
}

/**
 * @brief Commits a fully parsed document to the document table and index.
 * @param path Source path stored in the document metadata.
 * @param parsed Complete parsed state transferred into the build result.
 * @param result Build result updated with metadata, postings, and statistics.
 * @throws std::overflow_error If a statistic, identifier, or frequency exceeds
 * its supported range.
 * @throws std::invalid_argument If parsed posting order violates index
 * invariants.
 */
void commit_document(const std::filesystem::path &path, ParsedDocument parsed,
                     InMemoryBuildResult &result) {
        // Validate all aggregate counters before publishing the document.
        const auto indexed_files =
                checked_sum(result.stats.indexed_files, 1, "indexed_files");
        const auto indexed_bytes =
                checked_sum(result.stats.indexed_bytes,
                            parsed.read_stats.bytes_read, "indexed_bytes");
        const auto token_count =
                checked_sum(result.stats.token_count,
                            static_cast<std::uint64_t>(parsed.tokens.size()),
                            "token_count");

        // Assign the stable document ID before adding its ordered postings.
        const auto document_id =
                result.documents.add(path, parsed.metadata.file_size,
                                     parsed.metadata.modified_time_ns);

        for (const auto &token : parsed.tokens) {
                result.index.add_occurrence(token.term, document_id,
                                            token.position);
        }
        result.documents.set_token_count(
                document_id, static_cast<std::uint32_t>(parsed.tokens.size()));

        // Publish statistics only after metadata and postings are committed.
        result.stats.indexed_files = indexed_files;
        result.stats.indexed_bytes = indexed_bytes;
        result.stats.token_count = token_count;
}

} // namespace

InMemoryIndexBuilder::InMemoryIndexBuilder(InMemoryBuildOptions options)
    : options_(std::move(options)) {
        static_cast<void>(document::TextReader(options_.read_options));
        static_cast<void>(analysis::Tokenizer(options_.tokenizer_options));
}

InMemoryBuildResult
InMemoryIndexBuilder::build(const std::filesystem::path &source) const {
        InMemoryBuildResult result;

        // Discover and sort candidates first so successful document IDs remain
        // deterministic for a stable source tree.
        const filesystem::Scanner scanner(options_.scan_options);
        auto scan_result = scanner.scan(source);
        result.stats.scanned_files = scan_result.files.size();
        result.scan_errors = std::move(scan_result.errors);

        const document::TextReader reader(options_.read_options);
        for (const auto &path : scan_result.files) {
                // Parse into temporary state so ordinary per-file failures do
                // not leave partial documents or postings behind.
                std::optional<ParsedDocument> parsed;
                try {
                        parsed.emplace(parse_document(
                                path, reader, options_.tokenizer_options));
                } catch (const std::length_error &error) {
                        result.document_errors.push_back(
                                BuildError{path, error.what()});
                        ++result.stats.failed_files;
                        continue;
                } catch (const std::runtime_error &error) {
                        result.document_errors.push_back(
                                BuildError{path, error.what()});
                        ++result.stats.failed_files;
                        continue;
                }

                // Only a completely parsed file crosses the commit boundary.
                commit_document(path, std::move(*parsed), result);
        }
        return result;
}

} // namespace snowseek::index
