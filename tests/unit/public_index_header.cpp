/**
 * @file public_index_header.cpp
 * @brief Checks that the public indexing header is self-contained and usable.
 */

#include "snowseek/index.hpp"

#include <type_traits>

namespace snowseek::test {

/** @brief Exercises the complete public index result vocabulary. */
bool index_header_is_self_contained() {
        static_assert(std::is_move_constructible_v<IndexWriter>);
        IndexOptions options;
        options.temporary_directory = std::filesystem::path("workspace");
        const IndexResult result{
                .outcome = IndexOutcome::published,
                .revision = 1,
                .active_segments = 1,
                .changes = ChangeCounts{.added = 1},
                .metrics = BuildMetrics{.indexed_files = 1},
                .diagnostics = {Diagnostic{
                        DiagnosticStage::document, {}, "recoverable"}},
        };
        return result.outcome == IndexOutcome::published &&
               result.changes.added == 1 && result.metrics.indexed_files == 1 &&
               options.temporary_directory ==
                       std::filesystem::path("workspace");
}

} // namespace snowseek::test
