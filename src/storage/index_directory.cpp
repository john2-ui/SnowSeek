#include "snowseek/storage/index_file.hpp"

#include "storage/index_directory_internal.hpp"

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
#include <utility>
#include <vector>

namespace snowseek::storage {
namespace {

constexpr std::string_view kBuildPrefix = ".snowseek-build-";
constexpr std::string_view kManifestTemporaryPrefix = ".snowseek-manifest-";

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

/** @brief Flushes a regular file's contents and metadata to stable storage. */
void sync_file(const std::filesystem::path &path) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd == -1) {
                throw_errno("failed to open for fsync", path);
        }
        const int result = ::fsync(fd);
        const int saved_errno = errno;
        ::close(fd);
        if (result == -1) {
                errno = saved_errno;
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

/** @brief Creates a unique Manifest staging file in the index directory. */
[[nodiscard]] std::pair<int, std::filesystem::path>
create_manifest_temporary(const std::filesystem::path &directory) {
        auto pattern = (directory / ".snowseek-manifest-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const int fd = ::mkostemp(writable.data(), O_CLOEXEC);
        if (fd == -1) {
                throw_errno("failed to create Manifest temporary file",
                            directory);
        }
        return {fd, std::filesystem::path(writable.data())};
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
 * @brief Reads one complete generation, retrying only a concurrent commit.
 * @tparam Result Result returned by the Segment operation.
 * @tparam Operation Callable accepting the resolved Segment path.
 */
template <typename Result, typename Operation>
[[nodiscard]] Result with_active_segment(
        const std::filesystem::path &directory, Operation &&operation) {
        const auto manifest_path = directory / kManifestFileName;
        const auto legacy_path = directory / kSegmentFileName;
        for (int attempt = 0; attempt < 3; ++attempt) {
                if (path_exists(manifest_path, "Manifest")) {
                        const auto manifest = read_manifest_file(manifest_path);
                        const auto segment = directory / segment_file_name(
                                                           manifest
                                                                   .active_segments
                                                                   .front());
                        try {
                                return operation(segment);
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
                if (!path_exists(legacy_path, "legacy Segment")) {
                        if (path_exists(manifest_path, "Manifest")) {
                                continue;
                        }
                        throw std::runtime_error(
                                "index directory contains neither MANIFEST nor legacy Segment");
                }
                try {
                        return operation(legacy_path);
                } catch (...) {
                        const auto original = std::current_exception();
                        if (path_exists(manifest_path, "Manifest")) {
                                continue;
                        }
                        std::rethrow_exception(original);
                }
        }
        throw std::runtime_error(
                "index generation changed repeatedly while being opened");
}

} // namespace

LoadedIndex read_index_directory(const std::filesystem::path &directory) {
        return with_active_segment<LoadedIndex>(
                directory,
                [](const std::filesystem::path &path) {
                        return read_index_file(path);
                });
}

IndexFileStats
validate_index_directory(const std::filesystem::path &directory) {
        return with_active_segment<IndexFileStats>(
                directory,
                [](const std::filesystem::path &path) {
                        return validate_index_file(path);
                });
}

namespace detail {

void set_publish_observer(PublishObserver observer) noexcept {
        publish_observer = observer;
}

IndexDirectoryTransaction::IndexDirectoryTransaction(
        std::filesystem::path directory)
    : directory_(std::move(directory)) {
        directory_fd_ =
                ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd_ == -1) {
                throw_errno("failed to open index directory", directory_);
        }
        if (::flock(directory_fd_, LOCK_EX | LOCK_NB) == -1) {
                const int saved_errno = errno;
                ::close(directory_fd_);
                directory_fd_ = -1;
                errno = saved_errno;
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                        throw std::runtime_error(
                                "another writer is active for index directory: " +
                                directory_.string());
                }
                throw_errno("failed to lock index directory", directory_);
        }

        try {
                std::optional<IndexManifest> current_manifest;
                const auto manifest_path = directory_ / kManifestFileName;
                std::error_code inspect_error;
                if (std::filesystem::exists(manifest_path, inspect_error)) {
                        current_manifest = read_manifest_file(manifest_path);
                        old_segment_ = directory_ / segment_file_name(
                                                        current_manifest
                                                                ->active_segments
                                                                .front());
                        static_cast<void>(validate_index_file(old_segment_));
                } else if (inspect_error) {
                        throw std::runtime_error(
                                "failed to inspect current Manifest: " +
                                inspect_error.message());
                } else {
                        const auto legacy = directory_ / kSegmentFileName;
                        if (std::filesystem::exists(legacy, inspect_error)) {
                                static_cast<void>(validate_index_file(legacy));
                                old_segment_ = legacy;
                        } else if (inspect_error) {
                                throw std::runtime_error(
                                        "failed to inspect legacy Segment: " +
                                        inspect_error.message());
                        }
                }

                SegmentId maximum_seen = old_segment_.empty() ? 0 : 1;
                std::vector<std::filesystem::path> stale_paths;
                for (const auto &entry :
                     std::filesystem::directory_iterator(directory_)) {
                        const auto name = entry.path().filename().string();
                        if (const auto id = parse_segment_file_name(name)) {
                                maximum_seen = std::max(maximum_seen, *id);
                                if (entry.path() != old_segment_) {
                                        stale_paths.push_back(entry.path());
                                }
                        } else if (name.starts_with(kBuildPrefix) ||
                                   name.starts_with(kManifestTemporaryPrefix)) {
                                stale_paths.push_back(entry.path());
                        }
                }

                if (maximum_seen == std::numeric_limits<SegmentId>::max()) {
                        throw std::runtime_error("SegmentId space is exhausted");
                }
                const auto after_seen = maximum_seen + 1;
                if (current_manifest) {
                        if (current_manifest->generation ==
                            std::numeric_limits<std::uint64_t>::max()) {
                                throw std::runtime_error(
                                        "Manifest generation is exhausted");
                        }
                        generation_ = current_manifest->generation + 1;
                        segment_id_ = std::max(
                                current_manifest->next_segment_id, after_seen);
                } else {
                        generation_ = 1;
                        segment_id_ = old_segment_.empty()
                                              ? std::max<SegmentId>(1, after_seen)
                                              : std::max<SegmentId>(2, after_seen);
                }
                if (segment_id_ == std::numeric_limits<SegmentId>::max()) {
                        throw std::runtime_error("SegmentId space is exhausted");
                }

                for (const auto &path : stale_paths) {
                        std::error_code removal_error;
                        std::filesystem::remove_all(path, removal_error);
                        if (removal_error) {
                                throw std::runtime_error(
                                        "failed to remove stale SnowSeek path " +
                                        path.string() + ": " +
                                        removal_error.message());
                        }
                }
                if (!stale_paths.empty()) {
                        sync_directory(directory_fd_, directory_);
                }
        } catch (...) {
                ::flock(directory_fd_, LOCK_UN);
                ::close(directory_fd_);
                directory_fd_ = -1;
                throw;
        }
}

IndexDirectoryTransaction::~IndexDirectoryTransaction() {
        if (directory_fd_ != -1) {
                static_cast<void>(::flock(directory_fd_, LOCK_UN));
                static_cast<void>(::close(directory_fd_));
        }
}

SegmentId IndexDirectoryTransaction::segment_id() const noexcept {
        return segment_id_;
}

std::uint64_t IndexDirectoryTransaction::generation() const noexcept {
        return generation_;
}

std::filesystem::path IndexDirectoryTransaction::segment_path() const {
        return directory_ / segment_file_name(segment_id_);
}

std::vector<PublicationDiagnostic> IndexDirectoryTransaction::publish(
        const std::filesystem::path &candidate,
        std::string_view manifest_bytes) {
        const auto final_segment = segment_path();
        const auto final_manifest = directory_ / kManifestFileName;
        std::filesystem::path temporary_manifest;
        bool committed = false;

        try {
                sync_file(candidate);
                observe(PublishObservationPoint::candidate_synced);
                std::filesystem::rename(candidate, final_segment);
                observe(PublishObservationPoint::segment_renamed);
                sync_directory(directory_fd_, directory_);
                observe(PublishObservationPoint::segment_directory_synced);

                auto [manifest_fd, manifest_path] =
                        create_manifest_temporary(directory_);
                temporary_manifest = std::move(manifest_path);
                try {
                        write_all(manifest_fd, manifest_bytes,
                                  temporary_manifest);
                        if (::fsync(manifest_fd) == -1) {
                                throw_errno("failed to fsync",
                                            temporary_manifest);
                        }
                        if (::close(manifest_fd) == -1) {
                                manifest_fd = -1;
                                throw_errno("failed to close",
                                            temporary_manifest);
                        }
                        manifest_fd = -1;
                } catch (...) {
                        if (manifest_fd != -1) {
                                static_cast<void>(::close(manifest_fd));
                        }
                        throw;
                }
                static_cast<void>(read_manifest_file(temporary_manifest));
                observe(PublishObservationPoint::manifest_synced);
                std::filesystem::rename(temporary_manifest, final_manifest);
                committed = true;
                observe(PublishObservationPoint::manifest_renamed);
        } catch (...) {
                if (!committed) {
                        std::error_code ignored;
                        if (!temporary_manifest.empty()) {
                                std::filesystem::remove(temporary_manifest,
                                                        ignored);
                        }
                        std::filesystem::remove(final_segment, ignored);
                        try {
                                sync_directory(directory_fd_, directory_);
                        } catch (...) {
                        }
                }
                throw;
        }

        std::vector<PublicationDiagnostic> diagnostics;
        try {
                sync_directory(directory_fd_, directory_);
                observe(PublishObservationPoint::manifest_directory_synced);
        } catch (const std::exception &error) {
                diagnostics.push_back(PublicationDiagnostic{
                        directory_,
                        "new generation is visible, but its directory sync failed; old Segment was retained: " +
                                std::string(error.what())});
                return diagnostics;
        }

        bool cleanup_changed_directory = false;
        if (!old_segment_.empty() && old_segment_ != final_segment) {
                std::error_code removal_error;
                const bool removed =
                        std::filesystem::remove(old_segment_, removal_error);
                cleanup_changed_directory = removed;
                if (removal_error || !removed) {
                        diagnostics.push_back(PublicationDiagnostic{
                                old_segment_,
                                removal_error
                                        ? "new generation committed, but old Segment cleanup failed: " +
                                                  removal_error.message()
                                        : "new generation committed, but old Segment was not removed"});
                }
        }
        if (cleanup_changed_directory) {
                try {
                        sync_directory(directory_fd_, directory_);
                } catch (const std::exception &error) {
                        diagnostics.push_back(PublicationDiagnostic{
                                directory_,
                                "new generation committed, but old Segment cleanup was not synced: " +
                                        std::string(error.what())});
                }
        }
        return diagnostics;
}

} // namespace detail

} // namespace snowseek::storage
