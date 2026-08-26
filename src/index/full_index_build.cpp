/**
 * @file full_index_build.cpp
 * @brief Coordinates complete in-memory and persistent index builds.
 */

#include "index/index_builder_internal.hpp"

#include "common/checked_arithmetic.hpp"
#include "storage/index_file.hpp"
#include "storage/index_manifest.hpp"
#include "storage/index_file_internal.hpp"
#include "storage/segment_merge.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace snowseek::index::builder_detail {
namespace {

using common::detail::checked_add;
using common::detail::checked_multiply;

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

} // namespace

MemoryLimitExceeded::MemoryLimitExceeded()
    : std::runtime_error("memory limit exceeded during index build") {}

BuildMemoryBudget::BuildMemoryBudget(std::uint64_t limit_bytes)
    : limit_bytes_(limit_bytes) {}

void BuildMemoryBudget::resize(std::uint64_t current_bytes,
                               std::uint64_t requested_bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_bytes > used_bytes_) {
                throw std::logic_error("memory budget accounting underflow");
        }
        const auto other_bytes = used_bytes_ - current_bytes;
        if (requested_bytes > limit_bytes_ - other_bytes) {
                throw MemoryLimitExceeded();
        }
        used_bytes_ = other_bytes + requested_bytes;
        peak_bytes_ = std::max(peak_bytes_, used_bytes_);
}

void BuildMemoryBudget::release(std::uint64_t bytes) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        used_bytes_ -= std::min(bytes, used_bytes_);
}

std::uint64_t BuildMemoryBudget::peak_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_bytes_;
}

MemoryReservation::MemoryReservation(BuildMemoryBudget &budget) noexcept
    : budget_(&budget) {}

MemoryReservation::MemoryReservation(MemoryReservation &&other) noexcept
    : budget_(std::exchange(other.budget_, nullptr)),
      bytes_(std::exchange(other.bytes_, 0)) {}

MemoryReservation &
MemoryReservation::operator=(MemoryReservation &&other) noexcept {
        if (this != &other) {
                reset();
                budget_ = std::exchange(other.budget_, nullptr);
                bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
}

MemoryReservation::~MemoryReservation() { reset(); }

void MemoryReservation::resize(std::uint64_t bytes) {
        if (budget_ != nullptr) {
                budget_->resize(bytes_, bytes);
        }
        bytes_ = bytes;
}

void MemoryReservation::reset() noexcept {
        if (budget_ != nullptr) {
                budget_->release(bytes_);
        }
        budget_ = nullptr;
        bytes_ = 0;
}

std::uint64_t MemoryReservation::bytes() const noexcept { return bytes_; }

BuildWorkspace::BuildWorkspace(const std::filesystem::path &parent,
                               std::uint64_t budget_bytes)
    : budget_bytes_(budget_bytes) {
        auto pattern = (parent / ".snowseek-build-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
                throw std::system_error(errno, std::generic_category(),
                                        "failed to create build workspace");
        }
        path_ = std::move(pattern);
}

BuildWorkspace::~BuildWorkspace() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
}

const std::filesystem::path &BuildWorkspace::path() const noexcept {
        return path_;
}

std::uint64_t BuildWorkspace::remaining_bytes() const noexcept {
        return budget_bytes_ - used_bytes_;
}

void BuildWorkspace::require_additional(std::uint64_t bytes) const {
        if (bytes > remaining_bytes()) {
                throw std::runtime_error("temporary space budget exceeded");
        }
        std::error_code error;
        const auto space = std::filesystem::space(path_, error);
        if (error) {
                throw std::runtime_error(
                        "failed to inspect temporary filesystem space: " +
                        error.message());
        }
        if (bytes > space.available) {
                throw std::runtime_error(
                        "insufficient temporary filesystem space");
        }
}

void BuildWorkspace::add_file(std::uint64_t bytes) {
        if (bytes > remaining_bytes()) {
                throw std::runtime_error("temporary space budget exceeded");
        }
        used_bytes_ =
                checked_add(used_bytes_, bytes, "temporary workspace bytes");
        peak_bytes_ = std::max(peak_bytes_, used_bytes_);
}

void BuildWorkspace::note_transient_peak(std::uint64_t bytes) {
        const auto peak =
                checked_add(used_bytes_, bytes, "temporary workspace peak");
        if (peak > budget_bytes_) {
                throw std::runtime_error("temporary space budget exceeded");
        }
        peak_bytes_ = std::max(peak_bytes_, peak);
}

void BuildWorkspace::remove_file(const std::filesystem::path &path,
                                 std::uint64_t bytes) {
        std::error_code error;
        const bool removed = std::filesystem::remove(path, error);
        if (error || !removed) {
                throw std::runtime_error(
                        "failed to remove temporary Segment: " + path.string());
        }
        if (bytes > used_bytes_) {
                throw std::runtime_error(
                        "temporary workspace accounting underflow");
        }
        used_bytes_ -= bytes;
}

std::uint64_t BuildWorkspace::peak_bytes() const noexcept {
        return peak_bytes_;
}

std::uint64_t
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

std::uint64_t
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

std::uint64_t
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

std::uint64_t estimated_segment_source_bytes(
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

void update_estimated_peak(BuildMemoryStats &memory) {
        auto total = checked_add(memory.metadata_bytes,
                                 memory.reader_peak_bytes, "memory peak");
        total = checked_add(total, memory.token_peak_bytes, "memory peak");
        total = checked_add(total, memory.dictionary_bytes, "memory peak");
        memory.estimated_peak_bytes =
                checked_add(total, memory.posting_bytes, "memory peak");
}

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

void publish_candidate(storage::detail::IndexDirectoryTransaction &publication,
                       const std::filesystem::path &candidate,
                       std::vector<storage::SegmentId> active_segments,
                       BuildWorkspace &workspace,
                       BuildMemoryBudget &memory_budget,
                       PersistentBuildResult &result) {
        static_cast<void>(storage::validate_index_file(candidate));
        const storage::IndexManifest manifest{publication.generation(),
                                              publication.segment_id() + 1,
                                              std::move(active_segments)};
        const auto manifest_bytes = storage::encode_manifest(manifest);
        MemoryReservation manifest_memory(memory_budget);
        manifest_memory.resize(manifest_bytes.size());
        workspace.require_additional(manifest_bytes.size());
        workspace.note_transient_peak(manifest_bytes.size());
        const auto cleanup = publication.publish(candidate, manifest_bytes);
        for (const auto &diagnostic : cleanup) {
                result.cleanup_errors.push_back(
                        BuildError{diagnostic.path, diagnostic.message});
        }
        result.published = true;
        result.segment_id = publication.segment_id();
        result.manifest_generation = publication.generation();
        result.active_segment_count = manifest.active_segments.size();
        result.index_file = publication.segment_path();
        result.temporary_peak_bytes = workspace.peak_bytes();
        result.memory_peak_bytes = memory_budget.peak_bytes();
}

} // namespace snowseek::index::builder_detail

namespace snowseek::index {
namespace {

using builder_detail::BuildMemoryBudget;
using builder_detail::BuildWorkspace;
using builder_detail::MemoryLimitExceeded;
using builder_detail::MemoryReservation;
using builder_detail::ParsedDocument;
using common::detail::checked_add;
using common::detail::checked_multiply;

struct ParseFailure {
        std::string message; ///< Recoverable per-document diagnostic.
        BuildMemoryStats memory_stats; ///< Memory observed before failure.
};

using ParseTaskResult = std::variant<ParsedDocument, ParseFailure>;

struct ParseWaveResult {
        std::vector<ParseTaskResult> outcomes; ///< Results in scanner order.
        std::exception_ptr fatal_error; ///< Deferred hard worker failure.
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
 * @param sources Initial Segments in document order; at least two.
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
        static_cast<void>(merge_segment_group(candidate, sources, workspace));
        return checked_add(pass_count, 1, "merge pass count");
}

/**
 * @brief Parses one full-build candidate and classifies recoverable failures.
 * @param path Source candidate to parse.
 * @param options Reader and tokenizer configuration.
 * @param memory_budget Shared hard logical-memory budget.
 * @return Parsed document or recoverable failure with observed memory peaks.
 * @throws MemoryLimitExceeded If concurrent reservations exceed budget.
 */
[[nodiscard]] ParseTaskResult
parse_build_candidate(const std::filesystem::path &path,
                      const InMemoryBuildOptions &options,
                      BuildMemoryBudget &memory_budget) {
        BuildMemoryStats observed;
        try {
                return builder_detail::parse_document(
                        path, options.read_options, options.tokenizer_options,
                        &memory_budget, &observed);
        } catch (const MemoryLimitExceeded &) {
                throw;
        } catch (const std::length_error &error) {
                return ParseFailure{error.what(), observed};
        } catch (const std::runtime_error &error) {
                return ParseFailure{error.what(), observed};
        }
}

/**
 * @brief Parses one bounded wave concurrently and joins all started workers.
 * @param paths Deterministically ordered scanner candidates.
 * @param begin Index of the first candidate in this wave.
 * @param count Number of candidates to parse.
 * @param options Reader and tokenizer configuration.
 * @param memory_budget Shared hard logical-memory budget.
 * @return Path-aligned results plus any deferred memory failure.
 * @throws std::runtime_error If a worker cannot be started.
 */
[[nodiscard]] ParseWaveResult
parse_document_wave(const std::vector<std::filesystem::path> &paths,
                    std::size_t begin, std::size_t count,
                    const InMemoryBuildOptions &options,
                    BuildMemoryBudget &memory_budget) {
        std::vector<std::future<ParseTaskResult>> futures;
        futures.reserve(count);
        try {
                for (std::size_t offset = 0; offset < count; ++offset) {
                        const auto path = paths[begin + offset];
                        futures.push_back(std::async(
                                std::launch::async,
                                [path, &options, &memory_budget]() {
                                        return parse_build_candidate(
                                                path, options, memory_budget);
                                }));
                }
        } catch (const std::system_error &error) {
                throw std::runtime_error("failed to start index worker: " +
                                         std::string(error.what()));
        }

        ParseWaveResult result{std::vector<ParseTaskResult>(count), nullptr};
        for (std::size_t offset = 0; offset < count; ++offset) {
                try {
                        result.outcomes[offset] = futures[offset].get();
                } catch (const MemoryLimitExceeded &) {
                        result.fatal_error = std::current_exception();
                }
        }
        return result;
}

class SegmentBatch {
      public:
        /**
         * @brief Creates an empty batch with retained-memory accounting.
         * @param store_positions Whether emitted postings retain positions.
         * @param flush_threshold_bytes Retained bytes triggering a flush.
         * @param workspace Private workspace receiving completed Segments.
         * @param memory_budget Shared hard logical-memory budget.
         * @param scan_paths Scanner candidates retained through publication.
         * @param scan_errors Scanner diagnostics retained in the result.
         * @param document_errors Document diagnostics retained in the result.
         * @param stats Aggregate build statistics mutated on commit.
         */
        SegmentBatch(bool store_positions, std::uint64_t flush_threshold_bytes,
                     BuildWorkspace &workspace,
                     BuildMemoryBudget &memory_budget,
                     const std::vector<std::filesystem::path> &scan_paths,
                     const std::vector<filesystem::ScanError> &scan_errors,
                     const std::vector<BuildError> &document_errors,
                     InMemoryBuildStats &stats)
            : store_positions_(store_positions),
              flush_threshold_bytes_(flush_threshold_bytes),
              workspace_(workspace), memory_budget_(memory_budget),
              scan_paths_(scan_paths), scan_errors_(scan_errors),
              document_errors_(document_errors), stats_(stats),
              index_(store_positions), metadata_memory_(memory_budget),
              active_memory_(memory_budget) {
                refresh_metadata_memory();
        }

        /**
         * @brief Recharges metadata after a diagnostic vector changes.
         * @throws MemoryLimitExceeded If retained metadata exceeds budget.
         */
        void note_diagnostics_changed() { refresh_metadata_memory(); }

        /**
         * @brief Commits one parsed document and flushes at the memory
         * boundary.
         * @param path Logical relative path stored in the Segment.
         * @param parsed Complete parsed document consumed and reset in place.
         * @throws MemoryLimitExceeded If one document cannot cross the commit
         * window within budget.
         * @throws std::runtime_error If a required Segment flush fails.
         */
        void commit(const std::filesystem::path &path, ParsedDocument &parsed) {
                MemoryReservation commit_headroom(memory_budget_);
                try {
                        commit_headroom.resize(
                                parsed.memory_reservation.bytes());
                } catch (const MemoryLimitExceeded &) {
                        if (documents_.size() == 0) {
                                throw;
                        }
                        flush();
                        commit_headroom.resize(
                                parsed.memory_reservation.bytes());
                }

                builder_detail::commit_document(path, std::move(parsed),
                                                documents_, index_, stats_);
                parsed = {};
                const auto document_bytes = documents_.estimated_memory_bytes();
                peak_document_bytes_ =
                        std::max(peak_document_bytes_, document_bytes);
                const auto index_memory = index_.estimated_memory_usage();
                stats_.memory.dictionary_bytes =
                        std::max(stats_.memory.dictionary_bytes,
                                 index_memory.dictionary_bytes);
                stats_.memory.posting_bytes =
                        std::max(stats_.memory.posting_bytes,
                                 index_memory.posting_bytes);
                auto retained = checked_add(document_bytes,
                                            index_memory.dictionary_bytes,
                                            "active Segment memory");
                retained = checked_add(retained, index_memory.posting_bytes,
                                       "active Segment memory");
                active_memory_.resize(retained);
                commit_headroom.reset();
                if (retained >= flush_threshold_bytes_) {
                        flush();
                }
        }

        /**
         * @brief Writes the current nonempty batch and resets retained state.
         * @throws std::runtime_error If serialization or accounting fails.
         */
        void flush() {
                if (documents_.size() == 0) {
                        return;
                }
                const auto path = workspace_.path() /
                                  ("segment-" +
                                   std::to_string(segments_.size()) + ".idx");
                const auto stats = storage::detail::write_index_file_bounded(
                        path, documents_, index_, workspace_.remaining_bytes());
                workspace_.add_file(stats.file_size);
                segments_.push_back(
                        storage::detail::SegmentSource{path, stats});
                documents_ = {};
                index_ = InMemoryIndex(store_positions_);
                active_memory_.resize(0);
                refresh_metadata_memory();
        }

        /** @brief Returns the number of completed temporary Segments. */
        [[nodiscard]] std::size_t segment_count() const noexcept {
                return segments_.size();
        }

        /**
         * @brief Estimates retained descriptors before ownership is released.
         * @return Vector and path bytes used by Segment descriptors.
         */
        [[nodiscard]] std::uint64_t segment_source_bytes() const {
                return builder_detail::estimated_segment_source_bytes(
                        segments_);
        }

        /**
         * @brief Transfers completed Segments in document order.
         * @return Segment sources for final candidate assembly.
         */
        [[nodiscard]] std::vector<storage::detail::SegmentSource>
        release_segments() {
                return std::move(segments_);
        }

        /** @brief Returns the greatest retained document-table estimate. */
        [[nodiscard]] std::uint64_t peak_document_bytes() const noexcept {
                return peak_document_bytes_;
        }

      private:
        /**
         * @brief Recomputes scanner, diagnostic, and Segment metadata charge.
         * @throws MemoryLimitExceeded If replacement exceeds budget.
         */
        void refresh_metadata_memory() {
                auto bytes =
                        builder_detail::estimated_path_list_bytes(scan_paths_);
                bytes = checked_add(bytes,
                                    builder_detail::estimated_scan_error_bytes(
                                            scan_errors_),
                                    "metadata memory");
                bytes = checked_add(bytes,
                                    builder_detail::estimated_build_error_bytes(
                                            document_errors_),
                                    "metadata memory");
                bytes = checked_add(
                        bytes,
                        builder_detail::estimated_segment_source_bytes(
                                segments_),
                        "metadata memory");
                metadata_memory_.resize(bytes);
        }

        bool store_positions_{}; ///< Whether emitted postings retain token positions.
        std::uint64_t flush_threshold_bytes_{}; ///< Retained-byte flush boundary.
        BuildWorkspace &workspace_; ///< Workspace owning emitted Segments.
        BuildMemoryBudget &memory_budget_; ///< Shared logical-memory limit.
        const std::vector<std::filesystem::path> &scan_paths_; ///< Retained candidates.
        const std::vector<filesystem::ScanError> &scan_errors_; ///< Retained scan failures.
        const std::vector<BuildError> &document_errors_; ///< Retained parse failures.
        InMemoryBuildStats &stats_; ///< Aggregate statistics updated on commit.
        document::DocumentStore documents_; ///< Documents in the active batch.
        InMemoryIndex index_; ///< Postings in the active batch.
        std::vector<storage::detail::SegmentSource> segments_; ///< Completed batches.
        MemoryReservation metadata_memory_; ///< Charge for retained metadata.
        MemoryReservation active_memory_; ///< Charge for active batch content.
        std::uint64_t peak_document_bytes_{}; ///< Peak document-table bytes.
};

/**
 * @brief Records wave diagnostics, then commits successes in scan order.
 * @param paths Complete scanner path list.
 * @param begin Index of the first path represented by outcomes.
 * @param canonical_source Canonical root used to derive logical paths.
 * @param outcomes Parse results aligned with paths starting at begin.
 * @param fatal_error Deferred fatal worker failure, if one occurred.
 * @param result Persistent result receiving failures and memory maxima.
 * @param batch Segment batch receiving successful documents.
 */
void commit_document_wave(const std::vector<std::filesystem::path> &paths,
                          std::size_t begin,
                          const std::filesystem::path &canonical_source,
                          std::vector<ParseTaskResult> &outcomes,
                          const std::exception_ptr &fatal_error,
                          PersistentBuildResult &result, SegmentBatch &batch) {
        for (std::size_t offset = 0; offset < outcomes.size(); ++offset) {
                const auto &path = paths[begin + offset];
                if (const auto *failure =
                            std::get_if<ParseFailure>(&outcomes[offset])) {
                        builder_detail::observe_parse_memory(
                                failure->memory_stats, result.stats.memory);
                        result.document_errors.push_back(
                                BuildError{path, failure->message});
                        result.stats.failed_files = checked_add(
                                result.stats.failed_files, 1, "failed_files");
                        batch.note_diagnostics_changed();
                        continue;
                }
                builder_detail::observe_parse_memory(
                        std::get<ParsedDocument>(outcomes[offset]).memory_stats,
                        result.stats.memory);
        }
        if (fatal_error != nullptr) {
                std::rethrow_exception(fatal_error);
        }

        // Parse completion is concurrent; publication remains scanner ordered.
        for (std::size_t offset = 0; offset < outcomes.size(); ++offset) {
                auto *parsed = std::get_if<ParsedDocument>(&outcomes[offset]);
                if (parsed == nullptr) {
                        continue;
                }
                const auto &path = paths[begin + offset];
                const auto relative = path.lexically_relative(canonical_source);
                if (relative.empty() || relative.is_absolute()) {
                        throw std::runtime_error("indexed document is outside "
                                                 "the source root: " +
                                                 path.string());
                }
                batch.commit(relative, *parsed);
        }
}

struct CandidateBuildStats {
        std::uint64_t segment_source_bytes{}; ///< Bytes retained by Segment descriptors.
        std::uint64_t merge_memory_bytes{}; ///< Peak merge-buffer estimate.
};

/**
 * @brief Flushes all batches and assembles the final publishable Segment.
 * @param candidate Workspace path reserved for the final Segment.
 * @param batch Batch owner containing ordered temporary Segments.
 * @param options Position and merge configuration.
 * @param workspace Workspace enforcing temporary storage limits.
 * @param memory_budget Shared hard logical-memory budget.
 * @param result Build result receiving Segment and merge counts.
 * @return Metadata and merge-memory estimates for final statistics.
 */
[[nodiscard]] CandidateBuildStats prepare_build_candidate(
        const std::filesystem::path &candidate, SegmentBatch &batch,
        const PersistentBuildOptions &options, BuildWorkspace &workspace,
        BuildMemoryBudget &memory_budget, PersistentBuildResult &result) {
        batch.flush();
        result.temporary_segment_count = batch.segment_count();
        CandidateBuildStats build_stats{batch.segment_source_bytes(), 0};
        auto segments = batch.release_segments();

        if (segments.empty()) {
                const auto stats = storage::detail::write_index_file_bounded(
                        candidate, document::DocumentStore{},
                        InMemoryIndex(
                                options.in_memory_options.store_positions),
                        workspace.remaining_bytes());
                workspace.add_file(stats.file_size);
        } else if (segments.size() == 1) {
                std::filesystem::rename(segments.front().path, candidate);
        } else {
                MemoryReservation merge_reservation(memory_budget);
                build_stats.merge_memory_bytes =
                        storage::detail::estimate_segment_merge_memory(std::min(
                                segments.size(), options.merge_fan_in));
                merge_reservation.resize(build_stats.merge_memory_bytes);
                result.merge_pass_count =
                        merge_segment_levels(candidate, std::move(segments),
                                             options.merge_fan_in, workspace);
        }
        return build_stats;
}

/**
 * @brief Finalizes persistent build memory categories before publication.
 * @param scan_paths Scanner candidates retained throughout the build.
 * @param segment_source_bytes Peak retained Segment descriptor storage.
 * @param peak_document_bytes Peak retained document-table storage.
 * @param merge_memory_bytes Merge working-memory reservation.
 * @param result Build result whose memory statistics are updated.
 */
void finalize_persistent_memory_stats(
        const std::vector<std::filesystem::path> &scan_paths,
        std::uint64_t segment_source_bytes, std::uint64_t peak_document_bytes,
        std::uint64_t merge_memory_bytes, PersistentBuildResult &result) {
        auto metadata = builder_detail::estimated_path_list_bytes(scan_paths);
        metadata = checked_add(
                metadata,
                builder_detail::estimated_scan_error_bytes(result.scan_errors),
                "metadata memory");
        metadata = checked_add(metadata,
                               builder_detail::estimated_build_error_bytes(
                                       result.document_errors),
                               "metadata memory");
        metadata =
                checked_add(metadata, segment_source_bytes, "metadata memory");
        result.stats.memory.metadata_bytes = checked_add(
                metadata, std::max(peak_document_bytes, merge_memory_bytes),
                "metadata memory");
        builder_detail::update_estimated_peak(result.stats.memory);
}

} // namespace

PersistentBuildResult
IndexBuilder::build(const std::filesystem::path &source,
                    const std::filesystem::path &index_directory) const {
        if (!std::filesystem::exists(source)) {
                throw std::runtime_error("source path does not exist: " +
                                         source.string());
        }

        std::filesystem::create_directories(index_directory);
        storage::detail::IndexDirectoryTransaction publication(index_directory);
        const auto canonical_source = std::filesystem::weakly_canonical(source);
        const filesystem::Scanner scanner(
                options_.in_memory_options.scan_options);
        auto scan_result = scanner.scan(canonical_source);
        BuildWorkspace workspace(index_directory,
                                 options_.temporary_space_budget_bytes);
        BuildMemoryBudget memory_budget(options_.memory_budget_bytes);
        PersistentBuildResult result;
        result.segment_id = publication.segment_id();
        result.manifest_generation = publication.generation();
        result.stats.scanned_files = scan_result.files.size();
        result.scan_errors = std::move(scan_result.errors);
        result.worker_thread_count = options_.worker_thread_count;
        result.positions_enabled = options_.in_memory_options.store_positions;
        result.active_segment_count = 1;

        const auto effective_flush_threshold = std::min(
                options_.segment_flush_threshold_bytes,
                std::max<std::uint64_t>(1, options_.memory_budget_bytes / 2));
        SegmentBatch batch(options_.in_memory_options.store_positions,
                           effective_flush_threshold, workspace, memory_budget,
                           scan_result.files, result.scan_errors,
                           result.document_errors, result.stats);

        // Parse bounded waves concurrently, then commit in scanner order.
        for (std::size_t begin = 0; begin < scan_result.files.size();) {
                const auto count = std::min(options_.worker_thread_count,
                                            scan_result.files.size() - begin);
                auto wave = parse_document_wave(scan_result.files, begin, count,
                                                options_.in_memory_options,
                                                memory_budget);
                commit_document_wave(scan_result.files, begin, canonical_source,
                                     wave.outcomes, wave.fatal_error, result,
                                     batch);
                begin += count;
        }

        // Assemble all completed batches before crossing the publication
        // boundary.
        const auto candidate = workspace.path() / "candidate.idx";
        const auto candidate_stats = prepare_build_candidate(
                candidate, batch, options_, workspace, memory_budget, result);
        finalize_persistent_memory_stats(
                scan_result.files, candidate_stats.segment_source_bytes,
                batch.peak_document_bytes(), candidate_stats.merge_memory_bytes,
                result);

        result.added_files = result.stats.indexed_files;
        builder_detail::publish_candidate(publication, candidate,
                                          {publication.segment_id()}, workspace,
                                          memory_budget, result);
        return result;
}

} // namespace snowseek::index
