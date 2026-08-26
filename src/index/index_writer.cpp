#include "snowseek/index.hpp"

#include "index/index_builder.hpp"
#include "storage/index_file.hpp"

#include <stdexcept>
#include <utility>

namespace snowseek {
namespace {

/**
 * @brief Converts the public profile spelling to the internal build policy.
 * @param profile Public resource profile.
 * @return Matching internal profile.
 * @throws std::invalid_argument If profile does not name a public enumerator.
 */
[[nodiscard]] index::ResourceProfile internal_profile(ResourceProfile profile) {
        switch (profile) {
        case ResourceProfile::minimal:
                return index::ResourceProfile::minimal;
        case ResourceProfile::balanced:
                return index::ResourceProfile::balanced;
        case ResourceProfile::performance:
                return index::ResourceProfile::performance;
        }
        throw std::invalid_argument("unknown resource profile");
}

/**
 * @brief Resolves a public resource profile and its explicit overrides.
 * @param options Public writer configuration.
 * @return Complete internal persistent-build settings.
 */
[[nodiscard]] index::PersistentBuildOptions
internal_options(const IndexOptions &options) {
        auto resolved = index::persistent_build_options(
                internal_profile(options.profile));
        if (options.memory_limit_bytes.has_value()) {
                resolved.memory_budget_bytes = *options.memory_limit_bytes;
        }
        if (options.temporary_space_limit_bytes.has_value()) {
                resolved.temporary_space_budget_bytes =
                        *options.temporary_space_limit_bytes;
        }
        if (options.worker_threads.has_value()) {
                resolved.worker_thread_count = *options.worker_threads;
        }
        if (options.merge_fan_in.has_value()) {
                resolved.merge_fan_in = *options.merge_fan_in;
        }
        return resolved;
}

/**
 * @brief Adds one diagnostic category to the public result.
 * @param stage Public diagnostic stage.
 * @param errors Internal path-aware diagnostics to copy.
 * @param diagnostics Destination diagnostic list.
 */
void append_diagnostics(DiagnosticStage stage,
                        const std::vector<index::BuildError> &errors,
                        std::vector<Diagnostic> &diagnostics) {
        for (const auto &error : errors) {
                diagnostics.push_back(
                        Diagnostic{stage, error.path, error.message});
        }
}

/**
 * @brief Converts an internal operation result to the stable public shape.
 * @param result Complete internal result.
 * @return Grouped public operation information.
 */
[[nodiscard]] IndexResult
public_result(const index::PersistentBuildResult &result) {
        IndexResult converted;
        converted.outcome =
                !result.published ? IndexOutcome::unchanged
                                  : result.compacted ? IndexOutcome::compacted
                                                     : IndexOutcome::published;
        converted.revision = result.manifest_generation;
        converted.active_segments = result.active_segment_count;
        converted.changes = ChangeCounts{
                .added = result.added_files,
                .modified = result.modified_files,
                .removed = result.removed_files,
                .unchanged = result.unchanged_files,
                .matched = result.matched_files,
                .discarded_records = result.discarded_records,
        };
        converted.metrics = BuildMetrics{
                .scanned_files = result.stats.scanned_files,
                .indexed_files = result.stats.indexed_files,
                .failed_files = result.stats.failed_files,
                .indexed_bytes = result.stats.indexed_bytes,
                .token_count = result.stats.token_count,
                .peak_memory_bytes = result.memory_peak_bytes,
                .peak_temporary_bytes = result.temporary_peak_bytes,
                .temporary_segments = result.temporary_segment_count,
                .merge_passes = result.merge_pass_count,
                .worker_threads = result.worker_thread_count,
                .positions_enabled = result.positions_enabled,
        };
        for (const auto &error : result.scan_errors) {
                converted.diagnostics.push_back(
                        Diagnostic{DiagnosticStage::scan, error.path,
                                   error.error.message()});
        }
        append_diagnostics(DiagnosticStage::document, result.document_errors,
                           converted.diagnostics);
        append_diagnostics(DiagnosticStage::cleanup, result.cleanup_errors,
                           converted.diagnostics);
        append_diagnostics(DiagnosticStage::maintenance,
                           result.maintenance_errors, converted.diagnostics);
        return converted;
}

} // namespace

IndexWriter::IndexWriter(std::filesystem::path index_directory,
                         IndexOptions options)
    : index_directory_(std::move(index_directory)),
      options_(std::move(options)) {
        static_cast<void>(index::IndexBuilder(internal_options(options_)));
}

IndexResult IndexWriter::rebuild(const std::filesystem::path &source) {
        return public_result(index::IndexBuilder(internal_options(options_))
                                     .build(source, index_directory_));
}

IndexResult IndexWriter::update(const std::filesystem::path &source) {
        return public_result(index::IndexBuilder(internal_options(options_))
                                     .update(source, index_directory_));
}

IndexResult IndexWriter::remove(PathPatternSpan path_globs) {
        if (path_globs.empty()) {
                throw std::invalid_argument(
                        "remove requires at least one path pattern");
        }
        return public_result(
                index::IndexBuilder(internal_options(options_))
                        .remove(index_directory_,
                                std::vector<std::string>(path_globs.begin(),
                                                         path_globs.end())));
}

IndexResult IndexWriter::compact() {
        return public_result(index::IndexBuilder(internal_options(options_))
                                     .compact(index_directory_));
}

IndexStats validate_index(const std::filesystem::path &index_directory) {
        const auto stats = storage::validate_index_directory(index_directory);
        return IndexStats{
                .bytes = stats.file_size,
                .documents = stats.live_document_count,
                .segments = stats.segment_count,
                .tombstones = stats.tombstone_count,
                .terms = stats.term_count,
                .postings = stats.posting_count,
                .positions = stats.position_count,
        };
}

} // namespace snowseek
