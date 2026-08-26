/**
 * @file document_parser.cpp
 * @brief Extracts document metadata, tokens, and in-memory index records.
 */

#include "index/index_builder_internal.hpp"

#include "common/checked_arithmetic.hpp"
#include "storage/checksum.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <sys/stat.h>

namespace snowseek::index::builder_detail {
namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;

using common::detail::checked_add;
using common::detail::checked_multiply;

/**
 * @brief Converts a POSIX modification timestamp to Unix Epoch nanoseconds.
 * @param status File status containing the timestamp to convert.
 * @return Signed nanosecond timestamp.
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
 * @brief Estimates peak dynamic buffers used by one TextReader invocation.
 * @param options Reader configuration defining the source chunk size.
 * @return Conservative bytes for read, validation, output, and carry buffers.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
estimated_reader_peak_bytes(const document::TextReadOptions &options) {
        constexpr std::uint64_t maximum_utf8_sequence_bytes = 3;
        const auto source_buffer = checked_multiply(
                options.chunk_size, sizeof(char), "reader buffer");
        const auto validation_input = checked_add(
                source_buffer, maximum_utf8_sequence_bytes, "reader input");
        const auto validation_output = checked_multiply(
                validation_input, 3, "reader replacement output");
        return checked_add(
                checked_add(source_buffer, validation_input, "reader peak"),
                checked_add(validation_output, maximum_utf8_sequence_bytes,
                            "reader peak"),
                "reader peak");
}

/**
 * @brief Updates the peak bytes retained by one parsed document's tokens.
 * @param parsed Current document parse state.
 * @param tokenizer_options Tokenizer pending-buffer configuration.
 * @param memory Aggregate memory statistics to update.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
void record_token_peak(const ParsedDocument &parsed,
                       const analysis::TokenizerOptions &tokenizer_options,
                       BuildMemoryStats &memory) {
        auto bytes = checked_multiply(parsed.tokens.capacity(),
                                      sizeof(analysis::Token), "token vector");
        bytes = checked_add(bytes, parsed.token_term_bytes, "token terms");
        bytes = checked_add(bytes,
                            checked_multiply(tokenizer_options.max_token_length,
                                             sizeof(char), "tokenizer pending"),
                            "token peak");
        memory.token_peak_bytes = std::max(memory.token_peak_bytes, bytes);
}

} // namespace

FileMetadata read_metadata(const std::filesystem::path &path) {
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

ParsedDocument
parse_document(const std::filesystem::path &path,
               const document::TextReadOptions &read_options,
               const analysis::TokenizerOptions &tokenizer_options,
               BuildMemoryBudget *memory_budget,
               BuildMemoryStats *observed_memory) {
        ParsedDocument parsed;
        if (memory_budget != nullptr) {
                parsed.memory_reservation = MemoryReservation(*memory_budget);
        }

        parsed.memory_stats.reader_peak_bytes =
                std::max(parsed.memory_stats.reader_peak_bytes,
                         estimated_reader_peak_bytes(read_options));
        if (observed_memory != nullptr) {
                observed_memory->reader_peak_bytes =
                        std::max(observed_memory->reader_peak_bytes,
                                 parsed.memory_stats.reader_peak_bytes);
        }

        // Reject invalid or changing input before any tokens cross the commit
        // boundary.
        parsed.metadata = read_metadata(path);
        analysis::TokenizerSession tokenizer(tokenizer_options);
        record_token_peak(parsed, tokenizer_options, parsed.memory_stats);
        if (observed_memory != nullptr) {
                observed_memory->token_peak_bytes =
                        std::max(observed_memory->token_peak_bytes,
                                 parsed.memory_stats.token_peak_bytes);
        }
        parsed.memory_reservation.resize(checked_add(
                parsed.memory_stats.reader_peak_bytes,
                parsed.memory_stats.token_peak_bytes, "parsed document"));
        const document::TextReader reader(read_options);
        const analysis::TokenConsumer token_consumer =
                [&parsed, &tokenizer_options,
                 observed_memory](analysis::Token token) {
                        parsed.tokens.push_back(std::move(token));
                        parsed.token_term_bytes = checked_add(
                                parsed.token_term_bytes,
                                checked_multiply(
                                        parsed.tokens.back().term.capacity(),
                                        sizeof(char), "token term"),
                                "token terms");
                        record_token_peak(parsed, tokenizer_options,
                                          parsed.memory_stats);
                        if (observed_memory != nullptr) {
                                observed_memory->token_peak_bytes = std::max(
                                        observed_memory->token_peak_bytes,
                                        parsed.memory_stats.token_peak_bytes);
                        }
                        parsed.memory_reservation.resize(checked_add(
                                parsed.memory_stats.reader_peak_bytes,
                                parsed.memory_stats.token_peak_bytes,
                                "parsed document"));
                };
        storage::Crc32c content_checksum;
        parsed.read_stats = reader.read(
                path,
                [&tokenizer, &token_consumer](std::string_view chunk) {
                        tokenizer.push(chunk, token_consumer);
                },
                [&content_checksum](std::string_view bytes) {
                        content_checksum.update(bytes);
                });
        tokenizer.finish(token_consumer);

        const auto final_metadata = read_metadata(path);
        if (final_metadata.file_size != parsed.metadata.file_size ||
            final_metadata.modified_time_ns !=
                    parsed.metadata.modified_time_ns ||
            parsed.read_stats.bytes_read != parsed.metadata.file_size) {
                throw std::runtime_error(
                        "source file changed while it was being indexed: " +
                        path.string());
        }
        parsed.content_crc32c = content_checksum.value();

        if (parsed.tokens.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(
                        "document token count exceeds uint32_t");
        }
        return parsed;
}

void commit_document(const std::filesystem::path &path, ParsedDocument parsed,
                     document::DocumentStore &documents, InMemoryIndex &index,
                     InMemoryBuildStats &stats) {
        const auto indexed_files =
                checked_add(stats.indexed_files, 1, "indexed_files");
        const auto indexed_bytes =
                checked_add(stats.indexed_bytes, parsed.read_stats.bytes_read,
                            "indexed_bytes");
        const auto token_count =
                checked_add(stats.token_count,
                            static_cast<std::uint64_t>(parsed.tokens.size()),
                            "token_count");

        const auto document_id = documents.add(path, parsed.metadata.file_size,
                                               parsed.metadata.modified_time_ns,
                                               parsed.content_crc32c);
        for (const auto &token : parsed.tokens) {
                index.add_occurrence(token.term, document_id, token.position);
        }
        documents.set_token_count(
                document_id, static_cast<std::uint32_t>(parsed.tokens.size()));

        stats.indexed_files = indexed_files;
        stats.indexed_bytes = indexed_bytes;
        stats.token_count = token_count;
}

void observe_parse_memory(const BuildMemoryStats &observed,
                          BuildMemoryStats &aggregate) {
        aggregate.reader_peak_bytes = std::max(aggregate.reader_peak_bytes,
                                               observed.reader_peak_bytes);
        aggregate.token_peak_bytes =
                std::max(aggregate.token_peak_bytes, observed.token_peak_bytes);
}

} // namespace snowseek::index::builder_detail

namespace snowseek::index {

InMemoryIndexBuilder::InMemoryIndexBuilder(InMemoryBuildOptions options)
    : options_(std::move(options)) {
        static_cast<void>(document::TextReader(options_.read_options));
        static_cast<void>(analysis::Tokenizer(options_.tokenizer_options));
}

InMemoryBuildResult
InMemoryIndexBuilder::build(const std::filesystem::path &source) const {
        InMemoryBuildResult result;
        result.index = InMemoryIndex(options_.store_positions);

        const filesystem::Scanner scanner(options_.scan_options);
        auto scan_result = scanner.scan(source);
        result.stats.scanned_files = scan_result.files.size();
        result.scan_errors = std::move(scan_result.errors);

        for (const auto &path : scan_result.files) {
                std::optional<builder_detail::ParsedDocument> parsed;
                try {
                        parsed.emplace(builder_detail::parse_document(
                                path, options_.read_options,
                                options_.tokenizer_options, nullptr,
                                &result.stats.memory));
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
                builder_detail::commit_document(path, std::move(*parsed),
                                                result.documents, result.index,
                                                result.stats);
        }
        builder_detail::finalize_memory_stats(scan_result.files, result);
        return result;
}

} // namespace snowseek::index
