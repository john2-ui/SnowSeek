/**
 * @file index_maintenance.cpp
 * @brief Implements persistent index update, removal, and compaction.
 */

#include "index/index_builder_internal.hpp"

#include "common/checked_arithmetic.hpp"
#include "storage/checksum.hpp"
#include "storage/index_file.hpp"
#include "storage/index_manifest.hpp"
#include "storage/index_file_internal.hpp"
#include "storage/segment_merge.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <fnmatch.h>

namespace snowseek::index {
namespace {

using builder_detail::BuildMemoryBudget;
using builder_detail::BuildWorkspace;
using builder_detail::MemoryReservation;
using builder_detail::ParsedDocument;
using common::detail::checked_add;
using common::detail::checked_multiply;

struct FileFingerprint {
        builder_detail::FileMetadata metadata; ///< Stable size and modification time.
        std::uint32_t content_crc32c{}; ///< CRC32C of raw source bytes.
};

struct LiveDeltaRecord {
        std::filesystem::path relative_path; ///< Logical path stored in the index.
        std::filesystem::path source_path; ///< Physical file selected for parsing.
        FileFingerprint fingerprint; ///< Stable identity verified before commit.
};

struct TombstoneRecord {
        std::filesystem::path relative_path; ///< Logical path made invisible.
};

using PendingRecord = std::variant<LiveDeltaRecord, TombstoneRecord>;

enum class PublicationMode {
        append_delta,
        replace_active,
};

struct UpdatePlan {
        std::vector<PendingRecord> records; ///< Ordered delta records to publish.
        PublicationMode
                publication_mode{PublicationMode::append_delta}; ///< Manifest update strategy.
        std::uint64_t added_files{}; ///< New live paths in the plan.
        std::uint64_t modified_files{}; ///< Existing paths to replace.
        std::uint64_t removed_files{}; ///< Existing paths to tombstone.
        std::uint64_t unchanged_files{}; ///< Existing paths retained as-is.

        /** @brief Returns whether the plan must publish a new generation. */
        [[nodiscard]] bool requires_publication() const noexcept {
                return publication_mode == PublicationMode::replace_active ||
                       !records.empty();
        }
};

/**
 * @brief Reads one stable raw-file fingerprint without tokenizing the file.
 * @param path Regular source file to fingerprint.
 * @return Size, nanosecond mtime, and raw-byte CRC32C from one stable read.
 * @throws std::runtime_error If reading fails or metadata changes during read.
 */
[[nodiscard]] FileFingerprint
fingerprint_file(const std::filesystem::path &path) {
        const auto before = builder_detail::read_metadata(path);
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
                throw std::runtime_error(
                        "failed to open file for fingerprint: " +
                        path.string());
        }
        storage::Crc32c checksum;
        std::array<char, 64 * 1024> buffer{};
        std::uint64_t bytes_read = 0;
        while (input) {
                input.read(buffer.data(),
                           static_cast<std::streamsize>(buffer.size()));
                const auto count = input.gcount();
                if (count > 0) {
                        checksum.update(std::string_view(
                                buffer.data(),
                                static_cast<std::size_t>(count)));
                        bytes_read = checked_add(
                                bytes_read, static_cast<std::uint64_t>(count),
                                "fingerprint bytes");
                }
        }
        if (!input.eof()) {
                throw std::runtime_error("failed to read file fingerprint: " +
                                         path.string());
        }
        const auto after = builder_detail::read_metadata(path);
        if (before.file_size != after.file_size ||
            before.modified_time_ns != after.modified_time_ns ||
            bytes_read != before.file_size) {
                throw std::runtime_error(
                        "source file changed while it was fingerprinted: " +
                        path.string());
        }
        return FileFingerprint{before, checksum.value()};
}

/**
 * @brief Returns the logical path encoded by one delta operation.
 * @param record Live document or Tombstone operation.
 * @return Reference to the operation's relative index path.
 */
[[nodiscard]] const std::filesystem::path &
delta_record_path(const PendingRecord &record) noexcept {
        return std::visit(
                [](const auto &value) -> const std::filesystem::path & {
                        return value.relative_path;
                },
                record);
}

/**
 * @brief Orders delta operations by their portable logical path.
 * @param records Operations mutated into deterministic publication order.
 */
void sort_delta_records(std::vector<PendingRecord> &records) {
        std::sort(records.begin(), records.end(),
                  [](const PendingRecord &left, const PendingRecord &right) {
                          return delta_record_path(left).generic_string() <
                                 delta_record_path(right).generic_string();
                  });
}

/**
 * @brief Compares a scanned tree with the visible document table.
 * @param canonical_source Canonical source root used to derive logical paths.
 * @param scanned_files Deterministically ordered live source candidates.
 * @param current_documents Current visible document metadata.
 * @return Ordered live/Tombstone operations, publication mode, and counts.
 * @throws std::runtime_error If a candidate lies outside the source root or
 * cannot be fingerprinted stably.
 */
[[nodiscard]] UpdatePlan
plan_update(const std::filesystem::path &canonical_source,
            const std::vector<std::filesystem::path> &scanned_files,
            const document::DocumentStore &current_documents) {
        UpdatePlan plan;
        std::unordered_map<std::string, const document::DocumentMeta *>
                existing;
        bool requires_migration = false;
        for (const auto &document : current_documents.all()) {
                existing.emplace(document.path.generic_string(), &document);
                requires_migration = requires_migration ||
                                     !document.content_crc32c.has_value();
        }
        if (requires_migration) {
                plan.publication_mode = PublicationMode::replace_active;
        }

        std::unordered_set<std::string> seen;
        plan.records.reserve(scanned_files.size() + current_documents.size());
        for (const auto &path : scanned_files) {
                const auto relative = path.lexically_relative(canonical_source);
                if (relative.empty() || relative.is_absolute()) {
                        throw std::runtime_error("update candidate is outside "
                                                 "the source root: " +
                                                 path.string());
                }

                const auto key = relative.generic_string();
                const auto fingerprint = fingerprint_file(path);
                seen.insert(key);
                const auto old = existing.find(key);
                const bool added = old == existing.end();
                const bool changed =
                        !added &&
                        (old->second->file_size !=
                                 fingerprint.metadata.file_size ||
                         old->second->modified_time_ns !=
                                 fingerprint.metadata.modified_time_ns ||
                         !old->second->content_crc32c.has_value() ||
                         *old->second->content_crc32c !=
                                 fingerprint.content_crc32c);
                if (requires_migration || added || changed) {
                        plan.records.emplace_back(
                                LiveDeltaRecord{relative, path, fingerprint});
                        added ? ++plan.added_files : ++plan.modified_files;
                } else {
                        ++plan.unchanged_files;
                }
        }

        for (const auto &document : current_documents.all()) {
                if (seen.find(document.path.generic_string()) != seen.end()) {
                        continue;
                }
                ++plan.removed_files;
                if (!requires_migration) {
                        plan.records.emplace_back(
                                TombstoneRecord{document.path});
                }
        }
        sort_delta_records(plan.records);
        return plan;
}

/**
 * @brief Estimates retained document and posting storage for one loaded set.
 * @tparam Loaded LoadedSegment or LoadedIndex with documents and index members.
 * @param loaded Loaded document and posting set whose storage is estimated.
 * @return Classified document, dictionary, and posting bytes.
 */
template <typename Loaded>
[[nodiscard]] std::uint64_t
estimated_loaded_bytes(const Loaded &loaded) {
        const auto index_memory = loaded.index.estimated_memory_usage();
        return checked_add(loaded.documents.estimated_memory_bytes(),
                           checked_add(index_memory.dictionary_bytes,
                                       index_memory.posting_bytes,
                                       "loaded index memory"),
                           "loaded index memory");
}

/**
 * @brief Builds an in-memory v2 delta from deterministic operations.
 * @param records Path-sorted live records and Tombstones.
 * @param options Reader, tokenizer, and position configuration.
 * @param memory_budget Shared hard logical-memory budget.
 * @param result Aggregate operation statistics updated for live documents.
 * @return Complete delta ready for bounded serialization.
 * @throws std::runtime_error If parsing changes or fails.
 */
[[nodiscard]] storage::LoadedSegment
build_delta(const std::vector<PendingRecord> &records,
            const PersistentBuildOptions &options,
            BuildMemoryBudget &memory_budget, PersistentBuildResult &result) {
        storage::LoadedSegment delta;
        delta.index = InMemoryIndex(options.in_memory_options.store_positions);
        MemoryReservation retained(memory_budget);
        for (std::size_t begin = 0; begin < records.size();) {
                const auto count = std::min(options.worker_thread_count,
                                            records.size() - begin);
                std::vector<std::optional<std::future<ParsedDocument>>> tasks(
                        count);
                try {
                        for (std::size_t offset = 0; offset < count; ++offset) {
                                const auto *record =
                                        std::get_if<LiveDeltaRecord>(
                                                &records[begin + offset]);
                                if (record == nullptr) {
                                        continue;
                                }
                                tasks[offset].emplace(std::async(
                                        std::launch::async,
                                        [record, &options, &memory_budget]() {
                                                return builder_detail::parse_document(
                                                        record->source_path,
                                                        options.in_memory_options
                                                                .read_options,
                                                        options.in_memory_options
                                                                .tokenizer_options,
                                                        &memory_budget);
                                        }));
                        }
                } catch (const std::system_error &error) {
                        throw std::runtime_error(
                                "failed to start update worker: " +
                                std::string(error.what()));
                }

                // Workers finish independently; records cross the commit
                // boundary only in path order.
                for (std::size_t offset = 0; offset < count; ++offset) {
                        const auto &record = records[begin + offset];
                        if (const auto *tombstone =
                                    std::get_if<TombstoneRecord>(&record)) {
                                static_cast<void>(delta.documents.add_tombstone(
                                        tombstone->relative_path));
                        } else {
                                const auto &live =
                                        std::get<LiveDeltaRecord>(record);
                                auto parsed = tasks[offset]->get();
                                if (parsed.metadata.file_size !=
                                            live.fingerprint.metadata
                                                    .file_size ||
                                    parsed.metadata.modified_time_ns !=
                                            live.fingerprint.metadata
                                                    .modified_time_ns ||
                                    parsed.content_crc32c !=
                                            live.fingerprint.content_crc32c) {
                                        throw std::runtime_error(
                                                "source file changed after "
                                                "fingerprint: " +
                                                live.source_path.string());
                                }
                                builder_detail::observe_parse_memory(
                                        parsed.memory_stats,
                                        result.stats.memory);
                                builder_detail::commit_document(
                                        live.relative_path, std::move(parsed),
                                        delta.documents, delta.index,
                                        result.stats);
                        }
                        retained.resize(estimated_loaded_bytes(delta));
                }
                begin += count;
        }
        delta.stats.physical_document_count = delta.documents.size();
        delta.stats.live_document_count = result.stats.indexed_files;
        delta.stats.tombstone_count =
                delta.documents.size() - result.stats.indexed_files;
        return delta;
}

/**
 * @brief Serializes one loaded index while charging retained disk space.
 * @param path Workspace destination path.
 * @tparam Loaded LoadedSegment or LoadedIndex with documents and index members.
 * @param loaded Complete live or delta document and posting set to write.
 * @param workspace Temporary-space owner and budget.
 * @return Exact written Segment statistics.
 */
template <typename Loaded>
[[nodiscard]] storage::SegmentStats
write_loaded_candidate(const std::filesystem::path &path,
                       const Loaded &loaded,
                       BuildWorkspace &workspace) {
        const auto stats = storage::detail::write_index_file_bounded(
                path, loaded.documents, loaded.index,
                workspace.remaining_bytes());
        workspace.add_file(stats.file_size);
        return stats;
}

/**
 * @brief Builds and publishes one delta, compacting an oversized active set.
 * @param publication Locked transaction owning the new identifier.
 * @param current Current visible index, consumed only by compaction.
 * @param current_memory Reservation charging current retained storage.
 * @param records Deterministically ordered live records and Tombstones.
 * @param options Resource and encoding options for the operation.
 * @param index_directory Directory used for workspace and diagnostics.
 * @param publication_mode Append or replacement publication policy.
 * @param memory_budget Shared classified-memory budget.
 * @param result Operation result updated with publication statistics.
 */
void publish_delta(storage::detail::IndexDirectoryTransaction &publication,
                   storage::LoadedIndex &current,
                   MemoryReservation &current_memory,
                   const std::vector<PendingRecord> &records,
                   const PersistentBuildOptions &options,
                   const std::filesystem::path &index_directory,
                   PublicationMode publication_mode,
                   BuildMemoryBudget &memory_budget,
                   PersistentBuildResult &result) {
        auto operation_options = options;
        operation_options.in_memory_options.store_positions =
                current.index.stores_positions();
        BuildWorkspace workspace(index_directory,
                                 options.temporary_space_budget_bytes);
        auto delta =
                build_delta(records, operation_options, memory_budget, result);
        MemoryReservation delta_memory(memory_budget);
        delta_memory.resize(estimated_loaded_bytes(delta));
        result.temporary_segment_count = 1;
        auto candidate = workspace.path() / "candidate.idx";
        auto candidate_stats =
                write_loaded_candidate(candidate, delta, workspace);

        std::vector<storage::SegmentId> active;
        if (publication_mode == PublicationMode::replace_active) {
                active = {publication.segment_id()};
                result.compacted = true;
        } else {
                active = publication.active_segments();
                active.push_back(publication.segment_id());
                constexpr std::size_t maximum_active_segments = 16;
                if (active.size() > maximum_active_segments) {
                        try {
                                const auto physical_records = checked_add(
                                        current.stats.physical_document_count,
                                        delta.stats.physical_document_count,
                                        "physical document count");
                                MemoryReservation compact_memory(memory_budget);
                                compact_memory.resize(checked_multiply(
                                        checked_add(current_memory.bytes(),
                                                    delta_memory.bytes(),
                                                    "compaction input memory"),
                                        2, "compaction working memory"));
                                auto compacted = storage::detail::
                                        combine_loaded_index_with_segment(
                                                std::move(current),
                                                std::move(delta));
                                current_memory.reset();
                                delta_memory.reset();
                                compact_memory.resize(
                                        estimated_loaded_bytes(compacted));
                                result.discarded_records =
                                        physical_records -
                                        compacted.documents.size();
                                const auto compact_path =
                                        workspace.path() / "compact.idx";
                                const auto compact_stats =
                                        write_loaded_candidate(compact_path,
                                                               compacted,
                                                               workspace);
                                workspace.remove_file(
                                        candidate, candidate_stats.file_size);
                                candidate = compact_path;
                                candidate_stats = compact_stats;
                                active = {publication.segment_id()};
                                result.compacted = true;
                        } catch (const std::exception &error) {
                                current_memory.reset();
                                delta_memory.reset();
                                result.maintenance_errors.push_back(BuildError{
                                        index_directory,
                                        "automatic compaction failed; delta "
                                        "was still published: " +
                                                std::string(error.what())});
                        }
                }
        }
        current_memory.reset();
        delta_memory.reset();
        static_cast<void>(candidate_stats);
        builder_detail::publish_candidate(publication, candidate,
                                          std::move(active), workspace,
                                          memory_budget, result);
}

} // namespace

PersistentBuildResult
IndexBuilder::update(const std::filesystem::path &source,
                     const std::filesystem::path &index_directory) const {
        if (!std::filesystem::exists(source)) {
                throw std::runtime_error("source path does not exist: " +
                                         source.string());
        }
        if (!std::filesystem::is_directory(index_directory)) {
                throw std::runtime_error("index directory does not exist: " +
                                         index_directory.string());
        }

        storage::detail::IndexDirectoryTransaction publication(index_directory);
        BuildMemoryBudget memory_budget(options_.memory_budget_bytes);
        auto current = storage::read_index_directory(index_directory);
        MemoryReservation current_memory(memory_budget);
        current_memory.resize(estimated_loaded_bytes(current));
        PersistentBuildResult result;
        result.manifest_generation = publication.current_generation();
        result.active_segment_count = publication.active_segments().size();
        result.worker_thread_count = options_.worker_thread_count;
        result.positions_enabled = current.index.stores_positions();

        const auto canonical_source = std::filesystem::weakly_canonical(source);
        const filesystem::Scanner scanner(
                options_.in_memory_options.scan_options);
        const auto scan_result = scanner.scan(canonical_source);
        if (!scan_result.errors.empty()) {
                const auto &error = scan_result.errors.front();
                throw std::runtime_error(
                        "update scan failed: " + error.path.string() + ": " +
                        error.error.message());
        }
        result.stats.scanned_files = scan_result.files.size();

        const auto plan = plan_update(canonical_source, scan_result.files,
                                      current.documents);
        result.added_files = plan.added_files;
        result.modified_files = plan.modified_files;
        result.removed_files = plan.removed_files;
        result.unchanged_files = plan.unchanged_files;
        if (!plan.requires_publication()) {
                result.memory_peak_bytes = memory_budget.peak_bytes();
                return result;
        }

        publish_delta(publication, current, current_memory, plan.records,
                      options_, index_directory, plan.publication_mode,
                      memory_budget, result);
        return result;
}

PersistentBuildResult
IndexBuilder::remove(const std::filesystem::path &index_directory,
                     const std::vector<std::string> &glob_patterns) const {
        if (glob_patterns.empty() ||
            std::any_of(glob_patterns.begin(), glob_patterns.end(),
                        [](const std::string &pattern) {
                                return pattern.empty();
                        })) {
                throw std::invalid_argument(
                        "remove requires at least one nonempty path Glob");
        }
        storage::detail::IndexDirectoryTransaction publication(index_directory);
        BuildMemoryBudget memory_budget(options_.memory_budget_bytes);
        auto current = storage::read_index_directory(index_directory);
        MemoryReservation current_memory(memory_budget);
        current_memory.resize(estimated_loaded_bytes(current));
        PersistentBuildResult result;
        result.manifest_generation = publication.current_generation();
        result.active_segment_count = publication.active_segments().size();
        result.worker_thread_count = options_.worker_thread_count;
        result.positions_enabled = current.index.stores_positions();

        std::vector<PendingRecord> records;
        for (const auto &document : current.documents.all()) {
                const auto path = document.path.generic_string();
                const bool matches = std::any_of(
                        glob_patterns.begin(), glob_patterns.end(),
                        [&path](const std::string &pattern) {
                                return ::fnmatch(pattern.c_str(), path.c_str(),
                                                 0) == 0;
                        });
                if (matches) {
                        records.emplace_back(TombstoneRecord{document.path});
                }
        }
        sort_delta_records(records);
        result.matched_files = records.size();
        result.removed_files = records.size();
        if (records.empty()) {
                result.memory_peak_bytes = memory_budget.peak_bytes();
                return result;
        }

        publish_delta(publication, current, current_memory, records, options_,
                      index_directory, PublicationMode::append_delta,
                      memory_budget, result);
        return result;
}

PersistentBuildResult
IndexBuilder::compact(const std::filesystem::path &index_directory) const {
        storage::detail::IndexDirectoryTransaction publication(index_directory);
        BuildMemoryBudget memory_budget(options_.memory_budget_bytes);
        auto current = storage::read_index_directory(index_directory);
        MemoryReservation current_memory(memory_budget);
        current_memory.resize(estimated_loaded_bytes(current));
        PersistentBuildResult result;
        result.manifest_generation = publication.current_generation();
        result.active_segment_count = publication.active_segments().size();
        result.worker_thread_count = options_.worker_thread_count;
        result.positions_enabled = current.index.stores_positions();
        result.discarded_records = current.stats.physical_document_count -
                                   current.stats.live_document_count;
        if (publication.active_segments().size() == 1 &&
            result.discarded_records == 0) {
                result.memory_peak_bytes = memory_budget.peak_bytes();
                return result;
        }

        BuildWorkspace workspace(index_directory,
                                 options_.temporary_space_budget_bytes);
        const auto candidate = workspace.path() / "candidate.idx";
        static_cast<void>(
                write_loaded_candidate(candidate, current, workspace));
        current_memory.reset();
        result.compacted = true;
        result.temporary_segment_count = 1;
        builder_detail::publish_candidate(publication, candidate,
                                          {publication.segment_id()}, workspace,
                                          memory_budget, result);
        return result;
}

} // namespace snowseek::index
