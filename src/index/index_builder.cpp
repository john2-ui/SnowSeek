#include "snowseek/index/index_builder.hpp"

#include "snowseek/common/checked_arithmetic.hpp"
#include "snowseek/storage/index_file.hpp"
#include "storage/index_file_internal.hpp"
#include "storage/segment_merge.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

class MemoryLimitExceeded : public std::runtime_error {
      public:
        MemoryLimitExceeded()
            : std::runtime_error("memory limit exceeded during index build") {}
};

class BuildMemoryBudget {
      public:
        /** @brief Creates a classified logical-memory budget. */
        explicit BuildMemoryBudget(std::uint64_t limit_bytes)
            : limit_bytes_(limit_bytes) {}

        /**
         * @brief Replaces one reservation while preserving the total limit.
         * @param current_bytes Bytes currently charged by the caller.
         * @param requested_bytes Replacement charge requested by the caller.
         * @throws MemoryLimitExceeded If the replacement exceeds the limit.
         */
        void resize(std::uint64_t current_bytes,
                    std::uint64_t requested_bytes) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (current_bytes > used_bytes_) {
                        throw std::logic_error(
                                "memory budget accounting underflow");
                }
                const auto other_bytes = used_bytes_ - current_bytes;
                if (requested_bytes > limit_bytes_ - other_bytes) {
                        throw MemoryLimitExceeded();
                }
                used_bytes_ = other_bytes + requested_bytes;
                peak_bytes_ = std::max(peak_bytes_, used_bytes_);
        }

        /** @brief Releases a previously accepted reservation without throwing.
         */
        void release(std::uint64_t bytes) noexcept {
                std::lock_guard<std::mutex> lock(mutex_);
                used_bytes_ -= std::min(bytes, used_bytes_);
        }

        /** @brief Returns the greatest accepted concurrent reservation. */
        [[nodiscard]] std::uint64_t peak_bytes() const noexcept {
                std::lock_guard<std::mutex> lock(mutex_);
                return peak_bytes_;
        }

      private:
        std::uint64_t limit_bytes_{};
        mutable std::mutex mutex_;
        std::uint64_t used_bytes_{};
        std::uint64_t peak_bytes_{};
};

class MemoryReservation {
      public:
        MemoryReservation() = default;

        /** @brief Binds an initially empty charge to a shared budget. */
        explicit MemoryReservation(BuildMemoryBudget &budget) noexcept
            : budget_(&budget) {}

        MemoryReservation(const MemoryReservation &) = delete;
        MemoryReservation &operator=(const MemoryReservation &) = delete;

        /** @brief Transfers ownership of an accepted memory charge. */
        MemoryReservation(MemoryReservation &&other) noexcept
            : budget_(std::exchange(other.budget_, nullptr)),
              bytes_(std::exchange(other.bytes_, 0)) {}

        /** @brief Releases the old charge and takes another reservation. */
        MemoryReservation &operator=(MemoryReservation &&other) noexcept {
                if (this != &other) {
                        reset();
                        budget_ = std::exchange(other.budget_, nullptr);
                        bytes_ = std::exchange(other.bytes_, 0);
                }
                return *this;
        }

        /** @brief Releases the accepted charge. */
        ~MemoryReservation() { reset(); }

        /**
         * @brief Replaces the caller's logical-memory charge.
         * @param bytes New classified byte count.
         * @throws MemoryLimitExceeded If the shared limit would be exceeded.
         */
        void resize(std::uint64_t bytes) {
                if (budget_ != nullptr) {
                        budget_->resize(bytes_, bytes);
                }
                bytes_ = bytes;
        }

        /** @brief Releases all charged bytes while retaining no binding. */
        void reset() noexcept {
                if (budget_ != nullptr) {
                        budget_->release(bytes_);
                }
                budget_ = nullptr;
                bytes_ = 0;
        }

        /** @brief Returns bytes currently charged by this reservation. */
        [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

      private:
        BuildMemoryBudget *budget_{};
        std::uint64_t bytes_{};
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
        std::uint64_t token_term_bytes{};
        BuildMemoryStats memory_stats;
        MemoryReservation memory_reservation;
};

struct ParseTaskResult {
        std::optional<ParsedDocument> document;
        std::string error;
        BuildMemoryStats memory_stats;
};

using common::detail::checked_add;
using common::detail::checked_multiply;

class BuildWorkspace {
      public:
        /**
         * @brief Creates a unique private build directory below a destination.
         * @param parent Existing index directory that owns the workspace.
         * @param budget_bytes Maximum logical bytes retained in the workspace.
         * @throws std::system_error If Linux cannot create the workspace.
         */
        BuildWorkspace(const std::filesystem::path &parent,
                       std::uint64_t budget_bytes)
            : budget_bytes_(budget_bytes) {
                auto pattern = (parent / ".snowseek-build-XXXXXX").string();
                if (::mkdtemp(pattern.data()) == nullptr) {
                        throw std::system_error(
                                errno, std::generic_category(),
                                "failed to create build workspace");
                }
                path_ = std::move(pattern);
        }

        BuildWorkspace(const BuildWorkspace &) = delete;
        BuildWorkspace &operator=(const BuildWorkspace &) = delete;

        /** @brief Removes all unpublished files owned by this build. */
        ~BuildWorkspace() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
        }

        /** @brief Returns the private directory owned by this object. */
        [[nodiscard]] const std::filesystem::path &path() const noexcept {
                return path_;
        }

        /** @brief Returns bytes still permitted by the configured budget. */
        [[nodiscard]] std::uint64_t remaining_bytes() const noexcept {
                return budget_bytes_ - used_bytes_;
        }

        /**
         * @brief Checks budget and filesystem capacity before a temporary
         * write.
         * @param bytes Conservative additional bytes required by the operation.
         * @throws std::runtime_error If the budget, capacity query, or
         * available filesystem space cannot satisfy the request.
         */
        void require_additional(std::uint64_t bytes) const {
                if (bytes > remaining_bytes()) {
                        throw std::runtime_error(
                                "temporary space budget exceeded");
                }
                std::error_code error;
                const auto space = std::filesystem::space(path_, error);
                if (error) {
                        throw std::runtime_error("failed to inspect temporary "
                                                 "filesystem space: " +
                                                 error.message());
                }
                if (bytes > space.available) {
                        throw std::runtime_error(
                                "insufficient temporary filesystem space");
                }
        }

        /**
         * @brief Accounts for one completed file retained by the workspace.
         * @param bytes Logical file length to add.
         * @throws std::runtime_error If the completed file exceeds the budget.
         */
        void add_file(std::uint64_t bytes) {
                if (bytes > remaining_bytes()) {
                        throw std::runtime_error(
                                "temporary space budget exceeded");
                }
                used_bytes_ = checked_add(used_bytes_, bytes,
                                          "temporary workspace bytes");
                peak_bytes_ = std::max(peak_bytes_, used_bytes_);
        }

        /**
         * @brief Records transient spool and output bytes present during merge.
         * @param bytes Additional bytes that coexisted with retained files.
         * @throws std::runtime_error If the observed peak exceeds the budget.
         */
        void note_transient_peak(std::uint64_t bytes) {
                const auto peak = checked_add(used_bytes_, bytes,
                                              "temporary workspace peak");
                if (peak > budget_bytes_) {
                        throw std::runtime_error(
                                "temporary space budget exceeded");
                }
                peak_bytes_ = std::max(peak_bytes_, peak);
        }

        /**
         * @brief Deletes and releases one retained temporary file.
         * @param path File owned by this workspace.
         * @param bytes Accounted logical length of the file.
         * @throws std::runtime_error If deletion fails or accounting is
         * invalid.
         */
        void remove_file(const std::filesystem::path &path,
                         std::uint64_t bytes) {
                std::error_code error;
                const bool removed = std::filesystem::remove(path, error);
                if (error || !removed) {
                        throw std::runtime_error(
                                "failed to remove temporary Segment: " +
                                path.string());
                }
                if (bytes > used_bytes_) {
                        throw std::runtime_error(
                                "temporary workspace accounting underflow");
                }
                used_bytes_ -= bytes;
        }

        /** @brief Returns the observed logical workspace byte peak. */
        [[nodiscard]] std::uint64_t peak_bytes() const noexcept {
                return peak_bytes_;
        }

      private:
        std::filesystem::path path_;
        std::uint64_t budget_bytes_{};
        std::uint64_t used_bytes_{};
        std::uint64_t peak_bytes_{};
};

/**
 * @brief Merges and validates one ordered Segment group, then removes inputs.
 * @param output Unique intermediate or final candidate path.
 * @param sources Two or more Segments in global document order.
 * @param workspace Private workspace enforcing temporary storage limits.
 * @return Validated output Segment descriptor.
 * @throws std::runtime_error If capacity, merge, validation, or cleanup fails.
 */
[[nodiscard]] storage::detail::SegmentSource
merge_segment_group(const std::filesystem::path &output,
                    const std::vector<storage::detail::SegmentSource> &sources,
                    BuildWorkspace &workspace) {
        std::uint64_t input_bytes = 0;
        for (const auto &source : sources) {
                input_bytes = checked_add(input_bytes, source.stats.file_size,
                                          "merge input bytes");
        }
        const auto reserved_bytes =
                checked_multiply(input_bytes, 2, "merge temporary reservation");
        workspace.require_additional(reserved_bytes);

        workspace.note_transient_peak(
                storage::detail::merge_index_files(output, sources));

        const auto stats = storage::validate_index_file(output);
        workspace.add_file(stats.file_size);
        for (const auto &source : sources) {
                workspace.remove_file(source.path, source.stats.file_size);
        }
        return storage::detail::SegmentSource{output, stats};
}

/**
 * @brief Reduces Segments through bounded ordered merge levels.
 * @param candidate Final candidate path produced by the last level.
 * @param sources Initial Segments in document order; must contain at least two.
 * @param fan_in Maximum sources consumed by one merge.
 * @param workspace Private workspace owning every source and output.
 * @return Number of merge levels performed.
 * @throws std::runtime_error If any merge level cannot complete safely.
 */
[[nodiscard]] std::uint64_t
merge_segment_levels(const std::filesystem::path &candidate,
                     std::vector<storage::detail::SegmentSource> sources,
                     std::size_t fan_in, BuildWorkspace &workspace) {
        std::uint64_t pass_count = 0;

        // Reduce oversized levels into ordered intermediate Segments.
        while (sources.size() > fan_in) {
                std::vector<storage::detail::SegmentSource> next;
                next.reserve(1 + (sources.size() - 1) / fan_in);
                std::size_t group_number = 0;
                for (std::size_t begin = 0; begin < sources.size();
                     ++group_number) {
                        const auto count =
                                std::min(fan_in, sources.size() - begin);
                        const auto end = begin + count;
                        std::vector<storage::detail::SegmentSource> group(
                                sources.begin() +
                                        static_cast<std::ptrdiff_t>(begin),
                                sources.begin() +
                                        static_cast<std::ptrdiff_t>(end));
                        begin = end;
                        if (group.size() == 1) {
                                next.push_back(std::move(group.front()));
                                continue;
                        }
                        const auto output =
                                workspace.path() /
                                ("merge-" + std::to_string(pass_count) + "-" +
                                 std::to_string(group_number) + ".idx");
                        next.push_back(
                                merge_segment_group(output, group, workspace));
                }
                sources = std::move(next);
                pass_count = checked_add(pass_count, 1, "merge pass count");
        }

        // The remaining bounded group becomes the publishable candidate.
        static_cast<void>(merge_segment_group(candidate, sources, workspace));
        return checked_add(pass_count, 1, "merge pass count");
}

/**
 * @brief Estimates peak dynamic buffers used by one TextReader invocation.
 * @param options Reader configuration defining the source chunk size.
 * @return Conservative bytes for the read, validation input, output, and UTF-8
 * carry buffers.
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

/**
 * @brief Estimates dynamic bytes retained by one filesystem path.
 * @param path Path whose native-character capacity is inspected.
 * @return Conservative path character bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
estimated_path_bytes(const std::filesystem::path &path) {
        return checked_multiply(path.native().capacity(),
                                sizeof(std::filesystem::path::value_type),
                                "path storage");
}

/**
 * @brief Estimates retained scanner candidate storage.
 * @param paths Candidate paths preserved during the in-memory build.
 * @return Vector and path-character bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
estimated_path_list_bytes(const std::vector<std::filesystem::path> &paths) {
        auto bytes = checked_multiply(paths.capacity(),
                                      sizeof(std::filesystem::path),
                                      "scanner paths");
        for (const auto &path : paths) {
                bytes = checked_add(bytes, estimated_path_bytes(path),
                                    "scanner paths");
        }
        return bytes;
}

/**
 * @brief Estimates retained scanner diagnostic storage.
 * @param errors Scanner errors preserved in the build result.
 * @return Vector and path-character bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
estimated_scan_error_bytes(const std::vector<filesystem::ScanError> &errors) {
        auto bytes =
                checked_multiply(errors.capacity(),
                                 sizeof(filesystem::ScanError), "scan errors");
        for (const auto &error : errors) {
                bytes = checked_add(bytes, estimated_path_bytes(error.path),
                                    "scan errors");
        }
        return bytes;
}

/**
 * @brief Estimates retained per-document diagnostic storage.
 * @param errors Document errors preserved in the build result.
 * @return Vector, path, and message bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
estimated_build_error_bytes(const std::vector<BuildError> &errors) {
        auto bytes = checked_multiply(errors.capacity(), sizeof(BuildError),
                                      "document errors");
        for (const auto &error : errors) {
                bytes = checked_add(bytes, estimated_path_bytes(error.path),
                                    "document errors");
                bytes = checked_add(bytes,
                                    checked_multiply(error.message.capacity(),
                                                     sizeof(char),
                                                     "document error message"),
                                    "document errors");
        }
        return bytes;
}

/**
 * @brief Estimates retained temporary Segment descriptor storage.
 * @param segments Segment paths and logical statistics retained for merging.
 * @return Vector capacity and path character bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t estimated_segment_source_bytes(
        const std::vector<storage::detail::SegmentSource> &segments) {
        auto bytes = checked_multiply(segments.capacity(),
                                      sizeof(storage::detail::SegmentSource),
                                      "Segment sources");
        for (const auto &segment : segments) {
                bytes = checked_add(bytes, estimated_path_bytes(segment.path),
                                    "Segment sources");
        }
        return bytes;
}

/**
 * @brief Recomputes the conservative sum of all classified memory estimates.
 * @param memory Memory categories whose total is updated.
 * @throws std::overflow_error If their sum exceeds std::uint64_t.
 */
void update_estimated_peak(BuildMemoryStats &memory) {
        auto total = checked_add(memory.metadata_bytes,
                                 memory.reader_peak_bytes, "memory peak");
        total = checked_add(total, memory.token_peak_bytes, "memory peak");
        total = checked_add(total, memory.dictionary_bytes, "memory peak");
        memory.estimated_peak_bytes =
                checked_add(total, memory.posting_bytes, "memory peak");
}

/**
 * @brief Finalizes build memory categories after all documents are committed.
 * @param scan_paths Scanner candidates retained throughout the build.
 * @param result Completed build result whose statistics are updated.
 * @throws std::overflow_error If an estimate exceeds std::uint64_t.
 */
void finalize_memory_stats(const std::vector<std::filesystem::path> &scan_paths,
                           InMemoryBuildResult &result) {
        auto metadata = estimated_path_list_bytes(scan_paths);
        metadata = checked_add(metadata,
                               estimated_scan_error_bytes(result.scan_errors),
                               "metadata memory");
        metadata = checked_add(
                metadata, estimated_build_error_bytes(result.document_errors),
                "metadata memory");
        result.stats.memory.metadata_bytes =
                checked_add(metadata, result.documents.estimated_memory_bytes(),
                            "metadata memory");

        const auto index_memory = result.index.estimated_memory_usage();
        result.stats.memory.dictionary_bytes = index_memory.dictionary_bytes;
        result.stats.memory.posting_bytes = index_memory.posting_bytes;
        update_estimated_peak(result.stats.memory);
}

/**
 * @brief Reads and tokenizes one file without mutating the build result.
 * @param path Source file to parse.
 * @param reader Configured reader used to stream validated UTF-8 chunks.
 * @param read_options Reader buffer configuration used for memory estimation.
 * @param tokenizer_options Rules and limits applied during tokenization.
 * @param memory_budget Optional shared classified-memory budget.
 * @param observed_memory Optional aggregate receiving transient parse peaks.
 * @return Metadata, read statistics, and the complete token sequence.
 * @throws std::runtime_error If metadata, reading, or UTF-8 validation fails.
 * @throws std::length_error If a token exceeds the configured limit.
 * @throws std::overflow_error If the token count exceeds std::uint32_t.
 */
[[nodiscard]] ParsedDocument
parse_document(const std::filesystem::path &path,
               const document::TextReader &reader,
               const document::TextReadOptions &read_options,
               const analysis::TokenizerOptions &tokenizer_options,
               BuildMemoryBudget *memory_budget = nullptr,
               BuildMemoryStats *observed_memory = nullptr) {
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

        // Capture metadata before reading so a changed or invalid candidate is
        // rejected before any token work is retained.
        parsed.metadata = read_metadata(path);

        // Stream reader chunks through one tokenizer session so tokens may span
        // chunk boundaries while positions remain continuous.
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
 * @param documents Document table receiving the committed metadata.
 * @param index Inverted index receiving the committed occurrences.
 * @param stats Aggregate statistics updated after a complete commit.
 * @throws std::overflow_error If a statistic, identifier, or frequency exceeds
 * its supported range.
 * @throws std::invalid_argument If parsed posting order violates index
 * invariants.
 */
void commit_document(const std::filesystem::path &path, ParsedDocument parsed,
                     document::DocumentStore &documents, InMemoryIndex &index,
                     InMemoryBuildStats &stats) {
        // Validate all aggregate counters before publishing the document.
        const auto indexed_files =
                checked_add(stats.indexed_files, 1, "indexed_files");
        const auto indexed_bytes =
                checked_add(stats.indexed_bytes, parsed.read_stats.bytes_read,
                            "indexed_bytes");
        const auto token_count =
                checked_add(stats.token_count,
                            static_cast<std::uint64_t>(parsed.tokens.size()),
                            "token_count");

        // Assign the stable document ID before adding its ordered postings.
        const auto document_id =
                documents.add(path, parsed.metadata.file_size,
                              parsed.metadata.modified_time_ns);

        for (const auto &token : parsed.tokens) {
                index.add_occurrence(token.term, document_id, token.position);
        }
        documents.set_token_count(
                document_id, static_cast<std::uint32_t>(parsed.tokens.size()));

        // Publish statistics only after metadata and postings are committed.
        stats.indexed_files = indexed_files;
        stats.indexed_bytes = indexed_bytes;
        stats.token_count = token_count;
}

} // namespace

PersistentBuildOptions persistent_build_options(ResourceProfile profile) {
        PersistentBuildOptions options;
        switch (profile) {
        case ResourceProfile::minimal:
                options.memory_budget_bytes = 128ULL * 1024ULL * 1024ULL;
                options.segment_flush_threshold_bytes =
                        32ULL * 1024ULL * 1024ULL;
                options.worker_thread_count = 1;
                options.merge_fan_in = 4;
                options.in_memory_options.store_positions = false;
                break;
        case ResourceProfile::balanced:
                break;
        case ResourceProfile::performance:
                options.memory_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
                options.segment_flush_threshold_bytes =
                        512ULL * 1024ULL * 1024ULL;
                options.worker_thread_count = std::max<std::size_t>(
                        1, std::thread::hardware_concurrency());
                options.merge_fan_in = 32;
                break;
        }
        return options;
}

InMemoryIndexBuilder::InMemoryIndexBuilder(InMemoryBuildOptions options)
    : options_(std::move(options)) {
        static_cast<void>(document::TextReader(options_.read_options));
        static_cast<void>(analysis::Tokenizer(options_.tokenizer_options));
}

InMemoryBuildResult
InMemoryIndexBuilder::build(const std::filesystem::path &source) const {
        InMemoryBuildResult result;
        result.index = InMemoryIndex(options_.store_positions);

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
                                path, reader, options_.read_options,
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

                // Only a completely parsed file crosses the commit boundary.
                commit_document(path, std::move(*parsed), result.documents,
                                result.index, result.stats);
        }
        finalize_memory_stats(scan_result.files, result);
        return result;
}

IndexBuilder::IndexBuilder(PersistentBuildOptions options)
    : options_(std::move(options)) {
        if (options_.segment_flush_threshold_bytes == 0) {
                throw std::invalid_argument(
                        "segment flush threshold must be positive");
        }
        if (options_.temporary_space_budget_bytes == 0) {
                throw std::invalid_argument(
                        "temporary space budget must be positive");
        }
        if (options_.merge_fan_in < 2) {
                throw std::invalid_argument(
                        "merge fan-in must be at least two");
        }
        if (options_.memory_budget_bytes == 0) {
                throw std::invalid_argument("memory budget must be positive");
        }
        if (options_.worker_thread_count == 0) {
                throw std::invalid_argument(
                        "worker thread count must be positive");
        }
        static_cast<void>(InMemoryIndexBuilder(options_.in_memory_options));
}

PersistentBuildResult
IndexBuilder::build(const std::filesystem::path &source,
                    const std::filesystem::path &index_directory) const {
        if (!std::filesystem::exists(source)) {
                throw std::runtime_error("source path does not exist: " +
                                         source.string());
        }

        const auto canonical_source = std::filesystem::weakly_canonical(source);
        const filesystem::Scanner scanner(
                options_.in_memory_options.scan_options);
        auto scan_result = scanner.scan(canonical_source);
        std::filesystem::create_directories(index_directory);
        BuildWorkspace workspace(index_directory,
                                 options_.temporary_space_budget_bytes);
        BuildMemoryBudget memory_budget(options_.memory_budget_bytes);
        MemoryReservation metadata_memory(memory_budget);
        MemoryReservation active_batch_memory(memory_budget);
        PersistentBuildResult result;
        result.stats.scanned_files = scan_result.files.size();
        result.scan_errors = std::move(scan_result.errors);
        result.worker_thread_count = options_.worker_thread_count;
        result.positions_enabled = options_.in_memory_options.store_positions;

        document::DocumentStore batch_documents;
        InMemoryIndex batch_index(options_.in_memory_options.store_positions);
        std::vector<storage::detail::SegmentSource> segments;
        std::uint64_t peak_batch_document_bytes = 0;
        const auto effective_flush_threshold = std::min(
                options_.segment_flush_threshold_bytes,
                std::max<std::uint64_t>(1, options_.memory_budget_bytes / 2));

        /** Refreshes retained scanner, diagnostic, and Segment metadata. */
        const auto refresh_metadata_memory = [&]() {
                auto bytes = estimated_path_list_bytes(scan_result.files);
                bytes = checked_add(
                        bytes, estimated_scan_error_bytes(result.scan_errors),
                        "metadata memory");
                bytes = checked_add(
                        bytes,
                        estimated_build_error_bytes(result.document_errors),
                        "metadata memory");
                bytes = checked_add(bytes,
                                    estimated_segment_source_bytes(segments),
                                    "metadata memory");
                metadata_memory.resize(bytes);
        };
        refresh_metadata_memory();

        /** Flushes one complete document batch into the private workspace. */
        const auto flush_batch = [&]() {
                if (batch_documents.size() == 0) {
                        return;
                }
                const auto path =
                        workspace.path() /
                        ("segment-" + std::to_string(segments.size()) + ".idx");
                const auto stats = storage::detail::write_index_file_bounded(
                        path, batch_documents, batch_index,
                        workspace.remaining_bytes());
                workspace.add_file(stats.file_size);
                segments.push_back(storage::detail::SegmentSource{path, stats});
                batch_documents = {};
                batch_index = InMemoryIndex(
                        options_.in_memory_options.store_positions);
                active_batch_memory.resize(0);
                refresh_metadata_memory();
        };

        // Parse bounded waves concurrently, then commit in scanner order.
        for (std::size_t begin = 0; begin < scan_result.files.size();) {
                const auto count = std::min(options_.worker_thread_count,
                                            scan_result.files.size() - begin);
                std::vector<std::future<ParseTaskResult>> futures;
                futures.reserve(count);
                try {
                        for (std::size_t offset = 0; offset < count; ++offset) {
                                const auto path =
                                        scan_result.files[begin + offset];
                                futures.push_back(std::async(
                                        std::launch::async,
                                        [this, path, &memory_budget]() {
                                                BuildMemoryStats observed;
                                                const document::TextReader reader(
                                                        options_.in_memory_options
                                                                .read_options);
                                                try {
                                                        auto document = parse_document(
                                                                path, reader,
                                                                options_.in_memory_options
                                                                        .read_options,
                                                                options_.in_memory_options
                                                                        .tokenizer_options,
                                                                &memory_budget,
                                                                &observed);
                                                        return ParseTaskResult{
                                                                std::move(
                                                                        document),
                                                                {},
                                                                observed};
                                                } catch (
                                                        const MemoryLimitExceeded
                                                                &) {
                                                        throw;
                                                } catch (const std::length_error
                                                                 &error) {
                                                        return ParseTaskResult{
                                                                std::nullopt,
                                                                error.what(),
                                                                observed};
                                                } catch (
                                                        const std::runtime_error
                                                                &error) {
                                                        return ParseTaskResult{
                                                                std::nullopt,
                                                                error.what(),
                                                                observed};
                                                }
                                        }));
                        }
                } catch (const std::system_error &error) {
                        throw std::runtime_error(
                                "failed to start index worker: " +
                                std::string(error.what()));
                }

                std::vector<ParseTaskResult> outcomes(count);
                std::exception_ptr fatal_error;
                for (std::size_t offset = 0; offset < count; ++offset) {
                        const auto &path = scan_result.files[begin + offset];
                        try {
                                outcomes[offset] = futures[offset].get();
                        } catch (const MemoryLimitExceeded &) {
                                fatal_error = std::current_exception();
                                continue;
                        }
                        result.stats.memory.reader_peak_bytes = std::max(
                                result.stats.memory.reader_peak_bytes,
                                outcomes[offset]
                                        .memory_stats.reader_peak_bytes);
                        result.stats.memory.token_peak_bytes = std::max(
                                result.stats.memory.token_peak_bytes,
                                outcomes[offset].memory_stats.token_peak_bytes);
                        if (!outcomes[offset].error.empty()) {
                                result.document_errors.push_back(BuildError{
                                        path, outcomes[offset].error});
                                result.stats.failed_files =
                                        checked_add(result.stats.failed_files,
                                                    1, "failed_files");
                                refresh_metadata_memory();
                        }
                }
                if (fatal_error != nullptr) {
                        std::rethrow_exception(fatal_error);
                }

                for (std::size_t offset = 0; offset < count; ++offset) {
                        auto &parsed = outcomes[offset].document;
                        if (!parsed.has_value()) {
                                continue;
                        }
                        auto &document = *parsed;

                        MemoryReservation commit_headroom(memory_budget);
                        try {
                                commit_headroom.resize(
                                        document.memory_reservation.bytes());
                        } catch (const MemoryLimitExceeded &) {
                                if (batch_documents.size() == 0) {
                                        throw;
                                }
                                flush_batch();
                                commit_headroom.resize(
                                        document.memory_reservation.bytes());
                        }

                        const auto &path = scan_result.files[begin + offset];
                        const auto relative =
                                path.lexically_relative(canonical_source);
                        if (relative.empty() || relative.is_absolute()) {
                                throw std::runtime_error("indexed document is "
                                                         "outside the source "
                                                         "root: " +
                                                         path.string());
                        }
                        commit_document(relative, std::move(document),
                                        batch_documents, batch_index,
                                        result.stats);
                        parsed.reset();

                        const auto document_bytes =
                                batch_documents.estimated_memory_bytes();
                        peak_batch_document_bytes = std::max(
                                peak_batch_document_bytes, document_bytes);
                        const auto index_memory =
                                batch_index.estimated_memory_usage();
                        result.stats.memory.dictionary_bytes =
                                std::max(result.stats.memory.dictionary_bytes,
                                         index_memory.dictionary_bytes);
                        result.stats.memory.posting_bytes =
                                std::max(result.stats.memory.posting_bytes,
                                         index_memory.posting_bytes);
                        auto retained = checked_add(
                                document_bytes, index_memory.dictionary_bytes,
                                "active Segment memory");
                        retained = checked_add(retained,
                                               index_memory.posting_bytes,
                                               "active Segment memory");
                        active_batch_memory.resize(retained);
                        commit_headroom.reset();
                        if (retained >= effective_flush_threshold) {
                                flush_batch();
                        }
                }
                begin += count;
        }
        flush_batch();
        result.temporary_segment_count = segments.size();
        const auto segment_source_bytes =
                estimated_segment_source_bytes(segments);

        const auto index_file = index_directory / storage::kSegmentFileName;
        const auto candidate = workspace.path() / "candidate.idx";
        std::uint64_t merge_memory = 0;
        if (segments.empty()) {
                const auto stats = storage::detail::write_index_file_bounded(
                        candidate, document::DocumentStore{},
                        InMemoryIndex(
                                options_.in_memory_options.store_positions),
                        workspace.remaining_bytes());
                workspace.add_file(stats.file_size);
                static_cast<void>(storage::validate_index_file(candidate));
        } else if (segments.size() == 1) {
                std::filesystem::rename(segments.front().path, candidate);
                static_cast<void>(storage::validate_index_file(candidate));
        } else {
                MemoryReservation merge_reservation(memory_budget);
                merge_memory = storage::detail::estimate_segment_merge_memory(
                        std::min(segments.size(), options_.merge_fan_in));
                merge_reservation.resize(merge_memory);
                result.merge_pass_count =
                        merge_segment_levels(candidate, std::move(segments),
                                             options_.merge_fan_in, workspace);
        }
        result.temporary_peak_bytes = workspace.peak_bytes();
        result.memory_peak_bytes = memory_budget.peak_bytes();

        auto metadata = estimated_path_list_bytes(scan_result.files);
        metadata = checked_add(metadata,
                               estimated_scan_error_bytes(result.scan_errors),
                               "metadata memory");
        metadata = checked_add(
                metadata, estimated_build_error_bytes(result.document_errors),
                "metadata memory");
        metadata =
                checked_add(metadata, segment_source_bytes, "metadata memory");
        result.stats.memory.metadata_bytes = checked_add(
                metadata, std::max(peak_batch_document_bytes, merge_memory),
                "metadata memory");
        update_estimated_peak(result.stats.memory);

        // Publish only the fully validated candidate; the workspace owns every
        // other artifact and removes it on both success and failure.
        std::error_code rename_error;
        std::filesystem::rename(candidate, index_file, rename_error);
        if (rename_error) {
                throw std::runtime_error("failed to publish index file: " +
                                         rename_error.message());
        }
        result.index_file = index_file;
        return result;
}

} // namespace snowseek::index
