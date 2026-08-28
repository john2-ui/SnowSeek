/**
 * @file index_directory.cpp
 * @brief Loads, resolves, recovers, and durably publishes index directories.
 */

#include "storage/index_file.hpp"

#include "common/checked_arithmetic.hpp"
#include "storage/index_directory_internal.hpp"
#include "storage/index_file_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/file.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snowseek::storage {

namespace detail {

UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}

UniqueFd::~UniqueFd() {
        if (fd_ != -1) {
                static_cast<void>(::close(fd_));
        }
}

UniqueFd::UniqueFd(UniqueFd &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

UniqueFd &UniqueFd::operator=(UniqueFd &&other) noexcept {
        if (this != &other) {
                if (fd_ != -1) {
                        static_cast<void>(::close(fd_));
                }
                fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
}

int UniqueFd::get() const noexcept { return fd_; }

void UniqueFd::close_checked(const std::filesystem::path &path) {
        const int fd = std::exchange(fd_, -1);
        if (fd != -1 && ::close(fd) == -1) {
                const int saved_errno = errno;
                throw std::runtime_error("failed to close " + path.string() +
                                         ": " +
                                         std::strerror(saved_errno));
        }
}

} // namespace detail

namespace {

constexpr std::string_view kBuildPrefix = ".snowseek-build-";
constexpr std::string_view kManifestTemporaryPrefix = ".snowseek-manifest-";
constexpr std::string_view kSegmentTemporaryPrefix = ".snowseek-segment-";

thread_local detail::PublishObserver publish_observer = nullptr;

/** @brief Reports one publication boundary to an optional test observer. */
void observe(detail::PublishObservationPoint point) {
        if (publish_observer != nullptr) {
                publish_observer(point);
        }
}

/** @brief Throws a path-aware system error using the current errno value. */
[[noreturn]] void throw_errno(std::string_view operation,
                              const std::filesystem::path &path) {
        throw std::runtime_error(std::string(operation) + " " + path.string() +
                                 ": " + std::strerror(errno));
}

/**
 * @brief Flushes a regular file's contents and metadata to stable storage.
 * @param path Existing regular file to open and sync.
 * @throws std::runtime_error If opening or fsync fails.
 */
void sync_file(const std::filesystem::path &path) {
        detail::UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd.get() == -1) {
                throw_errno("failed to open for fsync", path);
        }
        if (::fsync(fd.get()) == -1) {
                throw_errno("failed to fsync", path);
        }
}

/** @brief Flushes directory entry changes through an already locked fd. */
void sync_directory(int directory_fd,
                    const std::filesystem::path &directory) {
        if (::fsync(directory_fd) == -1) {
                throw_errno("failed to fsync directory", directory);
        }
}

/** @brief Parses exactly a SnowSeek Segment filename without accepting junk. */
[[nodiscard]] std::optional<SegmentId>
parse_segment_file_name(std::string_view name) {
        constexpr std::string_view prefix = "segment-";
        constexpr std::string_view suffix = ".idx";
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) {
                return std::nullopt;
        }
        const auto digits = name.substr(
                prefix.size(), name.size() - prefix.size() - suffix.size());
        SegmentId id{};
        const auto result = std::from_chars(digits.data(),
                                            digits.data() + digits.size(), id);
        if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() ||
            id == 0 || segment_file_name(id) != name) {
                return std::nullopt;
        }
        return id;
}

/** @brief Writes every byte, retrying interrupted POSIX writes. */
void write_all(int fd, std::string_view bytes,
               const std::filesystem::path &path) {
        std::size_t offset = 0;
        while (offset != bytes.size()) {
                const auto written = ::write(fd, bytes.data() + offset,
                                             bytes.size() - offset);
                if (written == -1 && errno == EINTR) {
                        continue;
                }
                if (written <= 0) {
                        throw_errno("failed to write", path);
                }
                offset += static_cast<std::size_t>(written);
        }
}

struct ManifestTemporary {
        detail::UniqueFd fd; ///< Owned descriptor for the staging file.
        std::filesystem::path path; ///< Exact staging path in the index directory.
};

struct SegmentTemporary {
        detail::UniqueFd fd; ///< Owned descriptor for the staging file.
        std::filesystem::path path; ///< Exact staging path in the index directory.
};

/**
 * @brief Creates a unique Manifest staging file in the index directory.
 * @param directory Directory that will receive the committed Manifest.
 * @return Owned descriptor and exact staging path.
 * @throws std::runtime_error If the temporary file cannot be created.
 */
[[nodiscard]] ManifestTemporary
create_manifest_temporary(const std::filesystem::path &directory) {
        auto pattern = (directory / ".snowseek-manifest-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const int fd = ::mkostemp(writable.data(), O_CLOEXEC);
        if (fd == -1) {
                throw_errno("failed to create Manifest temporary file",
                            directory);
        }
        return ManifestTemporary{detail::UniqueFd(fd),
                                 std::filesystem::path(writable.data())};
}

/**
 * @brief Creates a unique Segment staging file in the index directory.
 * @param directory Directory that will receive the committed Segment.
 * @return Owned descriptor and exact staging path.
 * @throws std::runtime_error If the temporary file cannot be created.
 */
[[nodiscard]] SegmentTemporary
create_segment_temporary(const std::filesystem::path &directory) {
        auto pattern = (directory / ".snowseek-segment-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const int fd = ::mkostemp(writable.data(), O_CLOEXEC);
        if (fd == -1) {
                throw_errno("failed to create Segment temporary file",
                            directory);
        }
        return SegmentTemporary{detail::UniqueFd(fd),
                                std::filesystem::path(writable.data())};
}

/** @brief Reports whether a path exists without swallowing inspection errors. */
[[nodiscard]] bool path_exists(const std::filesystem::path &path,
                               std::string_view description) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
                throw std::runtime_error("failed to inspect " +
                                         std::string(description) + ": " +
                                         error.message());
        }
        return exists;
}

/**
 * @brief Reads one Manifest generation, retrying only a concurrent commit.
 * @tparam Result Result returned by the Segment operation.
 * @tparam Operation Callable accepting the resolved Segment paths.
 * @param directory Index directory containing MANIFEST.
 * @param operation Reader invoked for one stable active Segment set.
 * @return The operation result for a stable Manifest generation.
 * @throws std::runtime_error If no Manifest exists, metadata is invalid, or the
 * generation changes repeatedly.
 */
template <typename Result, typename Operation>
[[nodiscard]] Result with_active_segments(
        const std::filesystem::path &directory, Operation &&operation) {
        const auto manifest_path = directory / kManifestFileName;
        const auto legacy_path = directory / kSegmentFileName;
        for (int attempt = 0; attempt < 3; ++attempt) {
                if (path_exists(manifest_path, "Manifest")) {
                        const auto manifest = read_manifest_file(manifest_path);
                        std::vector<std::filesystem::path> segments;
                        segments.reserve(manifest.active_segments.size());
                        for (const auto id : manifest.active_segments) {
                                segments.push_back(directory /
                                                   segment_file_name(id));
                        }
                        try {
                                return operation(segments);
                        } catch (...) {
                                const auto original = std::current_exception();
                                const auto current =
                                        read_manifest_file(manifest_path);
                                if (current.generation != manifest.generation ||
                                    current.active_segments !=
                                            manifest.active_segments) {
                                        continue;
                                }
                                std::rethrow_exception(original);
                        }
                }
                const bool has_legacy =
                        path_exists(legacy_path, "legacy Segment");
                if (path_exists(manifest_path, "Manifest")) {
                        continue;
                }
                throw std::runtime_error(
                        has_legacy
                                ? "legacy index has no MANIFEST; rebuild the index"
                                : "index directory has no MANIFEST; rebuild the index");
        }
        throw std::runtime_error(
                "index generation changed repeatedly while being opened");
}

constexpr auto kDroppedDocument =
        std::numeric_limits<std::uint64_t>::max();

struct DocumentWinner {
        std::size_t segment{}; ///< Index of the newest Segment containing the path.
        document::DocumentId document{}; ///< Winning Segment-local identifier.
};

struct VisibilityPlan {
        document::DocumentStore documents; ///< Visible live records in global ID order.
        std::vector<std::vector<std::uint64_t>> remaps; ///< IDs; kDroppedDocument marks hidden records.
};

/** @brief Non-owning view of one loaded document and posting set. */
struct LoadedSetView {
        const document::DocumentStore &documents; ///< Borrowed document records.
        const index::InMemoryIndex &index; ///< Borrowed postings for that table.
};

/**
 * @brief Resolves newest-path visibility for ordered Segment document tables.
 * @tparam Segment LoadedIndex or LoadedDocumentTable with a documents member.
 * @param segments Active Segments in increasing persistent ID order.
 * @return Contiguous visible documents and one local-to-global map per Segment.
 */
template <typename Segment>
[[nodiscard]] VisibilityPlan
build_visibility_plan(const std::vector<Segment> &segments) {
        std::unordered_map<std::string, DocumentWinner> winners;
        for (std::size_t segment_index = 0; segment_index < segments.size();
             ++segment_index) {
                for (const auto &document :
                     segments[segment_index].documents.all()) {
                        winners[document.path.generic_string()] =
                                DocumentWinner{segment_index, document.id};
                }
        }

        VisibilityPlan plan;
        plan.remaps.reserve(segments.size());
        for (std::size_t segment_index = 0; segment_index < segments.size();
             ++segment_index) {
                const auto &documents = segments[segment_index].documents;
                auto &remap = plan.remaps.emplace_back(documents.size(),
                                                       kDroppedDocument);
                for (const auto &document : documents.all()) {
                        const auto winner =
                                winners.find(document.path.generic_string());
                        if (winner == winners.end() ||
                            winner->second.segment != segment_index ||
                            winner->second.document != document.id ||
                            document.state ==
                                    document::DocumentState::tombstone) {
                                continue;
                        }
                        const auto global = plan.documents.add(
                                document.path, document.file_size,
                                document.modified_time_ns,
                                document.content_crc32c);
                        plan.documents.set_token_count(global,
                                                       document.token_count);
                        remap[document.id] = global;
                }
        }
        return plan;
}

/**
 * @brief Adds physical counts from one validated Segment result.
 * @param aggregate Logical index statistics to update.
 * @param segment Validated statistics for one physical Segment.
 * @throws std::overflow_error If an aggregate count overflows.
 */
void add_physical_stats(IndexStats &aggregate, const SegmentStats &segment) {
        aggregate.file_size = common::detail::checked_add(
                aggregate.file_size, segment.file_size,
                "active Segment bytes");
        aggregate.tombstone_count = common::detail::checked_add(
                aggregate.tombstone_count, segment.tombstone_count,
                "Tombstone count");
        aggregate.physical_document_count = common::detail::checked_add(
                aggregate.physical_document_count,
                segment.physical_document_count,
                "physical document count");
}

/**
 * @brief Finalizes logical counts after newest-path posting resolution.
 * @param combined Resolved live documents and postings to measure.
 * @param segment_count Number of physical active Segments.
 * @throws std::overflow_error If a visible count overflows.
 */
void finish_logical_stats(LoadedIndex &combined,
                          std::uint64_t segment_count) {
        std::uint64_t posting_count = 0;
        std::uint64_t position_count = 0;
        for (const auto &term : combined.index.sorted_terms()) {
                const auto *postings = combined.index.find(term);
                posting_count = common::detail::checked_add(
                        posting_count, postings->size(), "visible posting count");
                for (const auto &posting : *postings) {
                        position_count = common::detail::checked_add(
                                position_count, posting.positions.size(),
                                "visible position count");
                }
        }
        combined.stats = IndexStats{
                .file_size = combined.stats.file_size,
                .live_document_count = combined.documents.size(),
                .tombstone_count = combined.stats.tombstone_count,
                .physical_document_count =
                        combined.stats.physical_document_count,
                .segment_count = segment_count,
                .term_count = combined.index.term_count(),
                .posting_count = posting_count,
                .position_count = position_count,
        };
}

/**
 * @brief Resolves loaded document and posting sets using newest-path visibility.
 * @param segments Ordered non-owning loaded-set views.
 * @param physical_stats Retained physical totals to preserve in the result.
 * @param segment_count Number of physical input Segments represented by views.
 * @return One contiguous live document table and merged inverted index.
 * @throws std::runtime_error If the set is empty or capabilities differ.
 */
[[nodiscard]] LoadedIndex
combine_loaded_sets(const std::vector<LoadedSetView> &segments,
                    IndexStats physical_stats,
                    std::uint64_t segment_count) {
        if (segments.empty()) {
                throw std::runtime_error("an index requires an active Segment");
        }
        const bool stores_positions = segments.front().index.stores_positions();
        for (const auto &segment : segments) {
                if (segment.index.stores_positions() != stores_positions) {
                        throw std::runtime_error(
                                "active Segments have different position capabilities");
                }
        }

        auto visibility = build_visibility_plan(segments);
        LoadedIndex combined;
        combined.documents = std::move(visibility.documents);
        combined.index = index::InMemoryIndex(stores_positions);
        combined.stats = physical_stats;
        for (std::size_t segment_index = 0; segment_index < segments.size();
             ++segment_index) {
                const auto &segment = segments[segment_index];
                for (const auto &term : segment.index.sorted_terms()) {
                        const auto *postings = segment.index.find(term);
                        for (const auto &posting : *postings) {
                                const auto global = visibility.remaps
                                                            [segment_index]
                                                            [posting.document_id];
                                if (global == kDroppedDocument) {
                                        continue;
                                }
                                combined.index.add_posting(
                                        term,
                                        static_cast<document::DocumentId>(
                                                global),
                                        posting.frequency, posting.positions);
                        }
                }
        }
        finish_logical_stats(combined, segment_count);
        return combined;
}

/**
 * @brief Resolves a set of fully loaded physical Segments.
 * @param segments Segment contents in increasing persistent ID order.
 * @return One visible logical index with aggregate physical statistics.
 * @throws std::runtime_error If the set is empty or capabilities differ.
 */
[[nodiscard]] LoadedIndex
combine_loaded_indexes_impl(const std::vector<LoadedSegment> &segments) {
        std::vector<LoadedSetView> views;
        views.reserve(segments.size());
        IndexStats physical_stats;
        for (const auto &segment : segments) {
                views.push_back(LoadedSetView{segment.documents,
                                              segment.index});
                add_physical_stats(physical_stats, segment.stats);
        }
        return combine_loaded_sets(
                views, physical_stats,
                static_cast<std::uint64_t>(segments.size()));
}

/**
 * @brief Applies one physical delta Segment to a resolved logical baseline.
 * @param base Visible logical baseline ordered before delta.
 * @param delta Newest physical Segment overriding matching baseline paths.
 * @return Resolved logical index with combined physical totals.
 * @throws std::runtime_error If position capabilities differ.
 * @throws std::overflow_error If the physical Segment count overflows.
 */
[[nodiscard]] LoadedIndex
combine_loaded_index_with_segment_impl(const LoadedIndex &base,
                                       const LoadedSegment &delta) {
        auto physical_stats = IndexStats{
                .file_size = base.stats.file_size,
                .live_document_count = 0,
                .tombstone_count = base.stats.tombstone_count,
                .physical_document_count =
                        base.stats.physical_document_count,
                .segment_count = 0,
                .term_count = 0,
                .posting_count = 0,
                .position_count = 0,
        };
        add_physical_stats(physical_stats, delta.stats);
        const auto segment_count = common::detail::checked_add(
                base.stats.segment_count, 1, "active Segment count");
        const std::vector<LoadedSetView> views{
                LoadedSetView{base.documents, base.index},
                LoadedSetView{delta.documents, delta.index},
        };
        return combine_loaded_sets(views, physical_stats, segment_count);
}

/**
 * @brief Resolves validated document tables and loads only visible Postings.
 * @param paths Active Segment paths in increasing persistent ID order.
 * @param segments Validated document tables matching paths.
 * @return One contiguous live document table and filtered inverted index.
 * @throws std::runtime_error If capabilities differ or a posting is invalid.
 * @throws std::logic_error If paths and tables have different sizes.
 */
[[nodiscard]] LoadedIndex combine_document_tables(
        const std::vector<std::filesystem::path> &paths,
        const std::vector<detail::LoadedDocumentTable> &segments) {
        if (segments.empty()) {
                throw std::runtime_error("an index requires an active Segment");
        }
        if (paths.size() != segments.size()) {
                throw std::logic_error(
                        "Segment paths do not match loaded document tables");
        }
        const bool stores_positions = segments.front().stores_positions;
        for (const auto &segment : segments) {
                if (segment.stores_positions != stores_positions) {
                        throw std::runtime_error(
                                "active Segments have different position capabilities");
                }
        }

        auto visibility = build_visibility_plan(segments);
        LoadedIndex combined;
        combined.documents = std::move(visibility.documents);
        combined.index = index::InMemoryIndex(stores_positions);
        for (std::size_t segment_index = 0; segment_index < segments.size();
             ++segment_index) {
                detail::append_remapped_postings(
                        paths[segment_index], segments[segment_index],
                        visibility.remaps[segment_index], combined.index);
                add_physical_stats(combined.stats,
                                   segments[segment_index].stats);
        }
        finish_logical_stats(combined, segments.size());
        return combined;
}

/**
 * @brief Loads and resolves active Segment paths in two bounded passes.
 * @param paths Active Segment paths in increasing SegmentId order.
 * @return Combined live index.
 * @throws std::runtime_error If any Segment is invalid.
 */
[[nodiscard]] LoadedIndex
load_active_segments(const std::vector<std::filesystem::path> &paths) {
        // Validate all physical records before publishing any query structure.
        std::vector<detail::LoadedDocumentTable> segments;
        segments.reserve(paths.size());
        for (const auto &path : paths) {
                segments.push_back(detail::load_document_table(path));
        }
        // Decode again while retaining only newest visible Postings.
        return combine_document_tables(paths, segments);
}

struct CurrentDirectoryState {
        std::optional<IndexManifest> manifest; ///< Current Manifest when one exists.
        std::vector<SegmentId> active_segments; ///< IDs protected by the current state.
        std::uint64_t generation{}; ///< Current Manifest generation, or zero if absent.
};

/**
 * @brief Loads the generation protected by the writer lock.
 * @param directory Locked index directory.
 * @return Manifest state, or a recognized fixed legacy Segment awaiting
 * replacement.
 * @throws std::runtime_error If Manifest or legacy-path inspection fails.
 */
[[nodiscard]] CurrentDirectoryState
load_current_directory_state(const std::filesystem::path &directory) {
        CurrentDirectoryState state;
        const auto manifest_path = directory / kManifestFileName;
        if (path_exists(manifest_path, "current Manifest")) {
                state.manifest = read_manifest_file(manifest_path);
                state.active_segments = state.manifest->active_segments;
                state.generation = state.manifest->generation;
                return state;
        }

        // A rebuild may replace an unreadable v1 fixed Segment. It remains in
        // place until the new Manifest has committed.
        const auto legacy_path = directory / kSegmentFileName;
        if (path_exists(legacy_path, "legacy Segment")) {
                std::error_code type_error;
                const bool regular =
                        std::filesystem::is_regular_file(legacy_path,
                                                         type_error);
                if (type_error) {
                        throw std::runtime_error(
                                "failed to inspect legacy Segment type: " +
                                type_error.message());
                }
                if (!regular) {
                        throw std::runtime_error(
                                "legacy Segment is not a regular file: " +
                                legacy_path.string());
                }
                state.active_segments = {1};
        }
        return state;
}

struct RecoveryScan {
        SegmentId maximum_seen{}; ///< Largest persistent Segment ID encountered.
        std::vector<std::filesystem::path> stale_paths; ///< Recognized orphan paths to remove.
};

/**
 * @brief Finds orphaned SnowSeek paths and the largest observed SegmentId.
 * @param directory Locked index directory to scan.
 * @param active_segments SegmentIds protected by the current generation.
 * @return Maximum persistent identifier and recognized stale paths.
 * @throws std::filesystem::filesystem_error If directory iteration fails.
 */
[[nodiscard]] RecoveryScan scan_recovery_paths(
        const std::filesystem::path &directory,
        const std::vector<SegmentId> &active_segments) {
        RecoveryScan scan;
        for (const auto id : active_segments) {
                scan.maximum_seen = std::max(scan.maximum_seen, id);
        }
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
                const auto name = entry.path().filename().string();
                if (const auto id = parse_segment_file_name(name)) {
                        scan.maximum_seen = std::max(scan.maximum_seen, *id);
                        if (std::find(active_segments.begin(),
                                      active_segments.end(), *id) ==
                            active_segments.end()) {
                                scan.stale_paths.push_back(entry.path());
                        }
                } else if (name.starts_with(kBuildPrefix) ||
                           name.starts_with(kSegmentTemporaryPrefix) ||
                           name.starts_with(kManifestTemporaryPrefix)) {
                        scan.stale_paths.push_back(entry.path());
                }
        }
        return scan;
}

struct TransactionIdentifiers {
        SegmentId segment_id{}; ///< Unused ID reserved for the candidate Segment.
        std::uint64_t generation{}; ///< Next Manifest generation to publish.
};

/**
 * @brief Allocates identifiers without reusing a committed or orphaned value.
 * @param state Current locked directory state.
 * @param maximum_seen Largest SegmentId present in the directory.
 * @return Next candidate SegmentId and Manifest generation.
 * @throws std::runtime_error If an identifier space is exhausted.
 */
[[nodiscard]] TransactionIdentifiers choose_transaction_identifiers(
        const CurrentDirectoryState &state, SegmentId maximum_seen) {
        if (maximum_seen == std::numeric_limits<SegmentId>::max()) {
                throw std::runtime_error("SegmentId space is exhausted");
        }
        const auto after_seen = maximum_seen + 1;
        if (!state.manifest.has_value()) {
                const auto segment_id = state.active_segments.empty()
                                                ? std::max<SegmentId>(1,
                                                                      after_seen)
                                                : std::max<SegmentId>(2,
                                                                      after_seen);
                if (segment_id == std::numeric_limits<SegmentId>::max()) {
                        throw std::runtime_error(
                                "SegmentId space is exhausted");
                }
                return TransactionIdentifiers{segment_id, 1};
        }
        if (state.manifest->generation ==
            std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("Manifest generation is exhausted");
        }
        const auto segment_id = std::max(state.manifest->next_segment_id,
                                         after_seen);
        if (segment_id == std::numeric_limits<SegmentId>::max()) {
                throw std::runtime_error("SegmentId space is exhausted");
        }
        return TransactionIdentifiers{segment_id,
                                      state.manifest->generation + 1};
}

/**
 * @brief Removes only recognized SnowSeek recovery leftovers.
 * @param paths Stale Segment, build, or Manifest staging paths.
 * @throws std::runtime_error If any path cannot be removed.
 */
void remove_recovery_paths(
        const std::vector<std::filesystem::path> &paths) {
        for (const auto &path : paths) {
                std::error_code removal_error;
                std::filesystem::remove_all(path, removal_error);
                if (removal_error) {
                        throw std::runtime_error(
                                "failed to remove stale SnowSeek path " +
                                path.string() + ": " +
                                removal_error.message());
                }
        }
}

/**
 * @brief Opens and exclusively locks an index directory.
 * @param directory Existing directory to lock.
 * @return Owned locked directory descriptor.
 * @throws std::runtime_error If opening or nonblocking locking fails.
 */
[[nodiscard]] detail::UniqueFd
open_locked_directory(const std::filesystem::path &directory) {
        detail::UniqueFd fd(
                ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (fd.get() == -1) {
                throw_errno("failed to open index directory", directory);
        }
        if (::flock(fd.get(), LOCK_EX | LOCK_NB) == -1) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                        throw std::runtime_error(
                                "another writer is active for index directory: " +
                                directory.string());
                }
                throw_errno("failed to lock index directory", directory);
        }
        return fd;
}

/**
 * @brief Validates a requested Manifest against locked transaction state.
 * @param bytes Serialized candidate Manifest.
 * @param generation Allocated Manifest generation.
 * @param segment_id Allocated candidate SegmentId.
 * @param active_segments Previously validated active SegmentIds.
 * @return Fully decoded requested Manifest.
 * @throws std::runtime_error If identifiers or selected Segments are invalid.
 */
[[nodiscard]] IndexManifest validate_publication_request(
        std::string_view bytes, std::uint64_t generation, SegmentId segment_id,
        const std::vector<SegmentId> &active_segments) {
        const auto manifest = decode_manifest(bytes);
        if (manifest.generation != generation ||
            manifest.next_segment_id != segment_id + 1) {
                throw std::runtime_error(
                        "published Manifest identifiers do not match the locked transaction");
        }
        if (std::find(manifest.active_segments.begin(),
                      manifest.active_segments.end(), segment_id) ==
            manifest.active_segments.end()) {
                throw std::runtime_error(
                        "published Manifest does not select the new Segment");
        }
        for (const auto id : manifest.active_segments) {
                if (id != segment_id &&
                    std::find(active_segments.begin(), active_segments.end(),
                              id) == active_segments.end()) {
                        throw std::runtime_error(
                                "published Manifest selects an unvalidated Segment");
                }
        }
        return manifest;
}

/**
 * @brief Writes, syncs, closes, and validates a Manifest staging file.
 * @param temporary Owned staging file and path.
 * @param bytes Prevalidated Manifest bytes to persist.
 * @throws std::runtime_error If writing, syncing, closing, or decoding fails.
 */
void write_synced_manifest(ManifestTemporary &temporary,
                           std::string_view bytes) {
        write_all(temporary.fd.get(), bytes, temporary.path);
        if (::fsync(temporary.fd.get()) == -1) {
                throw_errno("failed to fsync", temporary.path);
        }
        temporary.fd.close_checked(temporary.path);
        static_cast<void>(read_manifest_file(temporary.path));
}

/**
 * @brief Removes uncommitted publication files and best-effort syncs rollback.
 * @param candidate Candidate staging path before rename, if still present.
 * @param final_segment Renamed candidate path, if present.
 * @param temporary_manifest Manifest staging path, if created.
 * @param directory_fd Locked directory descriptor.
 * @param directory Directory to sync after removals.
 */
void rollback_publication(
        const std::filesystem::path &candidate,
        const std::filesystem::path &final_segment,
        const std::filesystem::path &temporary_manifest, int directory_fd,
        const std::filesystem::path &directory) noexcept {
        std::error_code ignored;
        if (!temporary_manifest.empty()) {
                std::filesystem::remove(temporary_manifest, ignored);
        }
        std::filesystem::remove(candidate, ignored);
        std::filesystem::remove(final_segment, ignored);
        try {
                sync_directory(directory_fd, directory);
        } catch (...) {
        }
}

/**
 * @brief Removes retired Segments after the new Manifest is durable.
 * @param directory Committed index directory.
 * @param directory_fd Locked directory descriptor.
 * @param previous_segments SegmentIds selected by the old generation.
 * @param active_segments SegmentIds selected by the new generation.
 * @return Nonfatal cleanup and cleanup-sync diagnostics.
 */
[[nodiscard]] std::vector<detail::PublicationDiagnostic>
cleanup_retired_segments(
        const std::filesystem::path &directory, int directory_fd,
        const std::vector<SegmentId> &previous_segments,
        const std::vector<SegmentId> &active_segments) {
        std::vector<detail::PublicationDiagnostic> diagnostics;
        bool changed = false;
        for (const auto old_id : previous_segments) {
                if (std::find(active_segments.begin(), active_segments.end(),
                              old_id) != active_segments.end()) {
                        continue;
                }
                const auto old_segment =
                        directory / segment_file_name(old_id);
                std::error_code removal_error;
                const bool removed =
                        std::filesystem::remove(old_segment, removal_error);
                changed = changed || removed;
                if (removal_error || !removed) {
                        diagnostics.push_back(detail::PublicationDiagnostic{
                                old_segment,
                                removal_error
                                        ? "new generation committed, but old Segment cleanup failed: " +
                                                  removal_error.message()
                                        : "new generation committed, but old Segment was not removed"});
                }
        }
        if (changed) {
                try {
                        sync_directory(directory_fd, directory);
                } catch (const std::exception &error) {
                        diagnostics.push_back(detail::PublicationDiagnostic{
                                directory,
                                "new generation committed, but old Segment cleanup was not synced: " +
                                        std::string(error.what())});
                }
        }
        return diagnostics;
}

} // namespace

LoadedIndex read_index_directory(const std::filesystem::path &directory) {
        return with_active_segments<LoadedIndex>(
                directory,
                [](const std::vector<std::filesystem::path> &paths) {
                        return load_active_segments(paths);
                });
}

IndexStats
validate_index_directory(const std::filesystem::path &directory) {
        return with_active_segments<IndexStats>(
                directory,
                [](const std::vector<std::filesystem::path> &paths) {
                        return load_active_segments(paths).stats;
                });
}

namespace detail {

LoadedIndex combine_loaded_indexes(std::vector<LoadedSegment> segments) {
        return combine_loaded_indexes_impl(segments);
}

LoadedIndex combine_loaded_index_with_segment(LoadedIndex base,
                                              LoadedSegment delta) {
        return combine_loaded_index_with_segment_impl(base, delta);
}

void set_publish_observer(PublishObserver observer) noexcept {
        publish_observer = observer;
}

IndexDirectoryTransaction::IndexDirectoryTransaction(
        std::filesystem::path directory)
    : directory_(std::move(directory)),
      directory_fd_(open_locked_directory(directory_)) {
        const auto state = load_current_directory_state(directory_);
        const auto recovery =
                scan_recovery_paths(directory_, state.active_segments);
        const auto identifiers = choose_transaction_identifiers(
                state, recovery.maximum_seen);

        active_segments_ = state.active_segments;
        current_generation_ = state.generation;
        segment_id_ = identifiers.segment_id;
        generation_ = identifiers.generation;

        remove_recovery_paths(recovery.stale_paths);
        if (!recovery.stale_paths.empty()) {
                sync_directory(directory_fd_.get(), directory_);
        }
}

IndexDirectoryTransaction::~IndexDirectoryTransaction() = default;

SegmentId IndexDirectoryTransaction::segment_id() const noexcept {
        return segment_id_;
}

std::uint64_t IndexDirectoryTransaction::current_generation() const noexcept {
        return current_generation_;
}

const std::vector<SegmentId> &
IndexDirectoryTransaction::active_segments() const noexcept {
        return active_segments_;
}

std::uint64_t IndexDirectoryTransaction::generation() const noexcept {
        return generation_;
}

std::filesystem::path IndexDirectoryTransaction::segment_path() const {
        return directory_ / segment_file_name(segment_id_);
}

void IndexDirectoryTransaction::validate_current_segments() const {
        if (current_generation_ == 0) {
                return;
        }
        for (const auto id : active_segments_) {
                static_cast<void>(validate_index_file(
                        directory_ / segment_file_name(id)));
        }
}

LoadedIndex IndexDirectoryTransaction::read_current_index() const {
        if (current_generation_ == 0) {
                throw std::runtime_error(
                        active_segments_.empty()
                                ? "index directory has no MANIFEST; rebuild the index"
                                : "legacy index has no MANIFEST; rebuild the index");
        }
        std::vector<std::filesystem::path> paths;
        paths.reserve(active_segments_.size());
        for (const auto id : active_segments_) {
                paths.push_back(directory_ / segment_file_name(id));
        }
        return load_active_segments(paths);
}

std::filesystem::path IndexDirectoryTransaction::stage_candidate(
        const std::filesystem::path &candidate) const {
        auto staging = create_segment_temporary(directory_);
        try {
                staging.fd.close_checked(staging.path);
                std::filesystem::copy_file(
                        candidate, staging.path,
                        std::filesystem::copy_options::overwrite_existing);
        } catch (...) {
                std::error_code ignored;
                std::filesystem::remove(staging.path, ignored);
                throw;
        }
        return staging.path;
}

std::vector<PublicationDiagnostic> IndexDirectoryTransaction::publish(
        const std::filesystem::path &candidate,
        std::string_view manifest_bytes) {
        const auto final_segment = segment_path();
        const auto final_manifest = directory_ / kManifestFileName;
        IndexManifest new_manifest;
        std::filesystem::path temporary_manifest;
        bool committed = false;

        try {
                new_manifest = validate_publication_request(
                        manifest_bytes, generation_, segment_id_,
                        active_segments_);
                // Persist the candidate before exposing its final name.
                sync_file(candidate);
                observe(PublishObservationPoint::candidate_synced);
                std::filesystem::rename(candidate, final_segment);
                observe(PublishObservationPoint::segment_renamed);
                sync_directory(directory_fd_.get(), directory_);
                observe(PublishObservationPoint::segment_directory_synced);

                // The Manifest rename below is the sole logical commit point.
                auto staging = create_manifest_temporary(directory_);
                temporary_manifest = staging.path;
                write_synced_manifest(staging, manifest_bytes);
                observe(PublishObservationPoint::manifest_synced);
                std::filesystem::rename(temporary_manifest, final_manifest);
                committed = true;
                observe(PublishObservationPoint::manifest_renamed);
        } catch (...) {
                if (!committed) {
                        rollback_publication(candidate, final_segment,
                                             temporary_manifest,
                                             directory_fd_.get(), directory_);
                }
                throw;
        }

        std::vector<PublicationDiagnostic> diagnostics;
        try {
                // Make the committed Manifest directory entry crash-durable.
                sync_directory(directory_fd_.get(), directory_);
                observe(PublishObservationPoint::manifest_directory_synced);
        } catch (const std::exception &error) {
                diagnostics.push_back(PublicationDiagnostic{
                        directory_,
                        "new generation is visible, but its directory sync failed; old Segment was retained: " +
                                std::string(error.what())});
                return diagnostics;
        }

        // Retirement is safe only after the committed Manifest is durable.
        return cleanup_retired_segments(
                directory_, directory_fd_.get(), active_segments_,
                new_manifest.active_segments);
}

} // namespace detail

} // namespace snowseek::storage
