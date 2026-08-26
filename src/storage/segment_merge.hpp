/**
 * @file segment_merge.hpp
 * @brief Declares bounded merging of immutable Segment files.
 */

#pragma once

#include "storage/index_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace snowseek::storage::detail {

struct SegmentSource {
        std::filesystem::path path; ///< Existing validated Segment file.
        SegmentStats stats; ///< Validated physical statistics for the Segment.
};

/**
 * @brief Estimates bounded cursor and copy-buffer memory for one merge group.
 * @param source_count Number of input Segments opened by the group.
 * @return Classified merge working-memory bytes.
 * @throws std::overflow_error If the estimate exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
estimate_segment_merge_memory(std::size_t source_count);

/**
 * @brief Merges ordered temporary v2 Segments into one v2 file.
 * @param output Candidate output path whose parent is used for spool files.
 * @param sources Nonempty Segments in global document order.
 * @return Actual peak spool-plus-output bytes retained during the merge.
 * @throws std::runtime_error If an input is malformed, a format limit is exceeded,
 * or temporary/output I/O fails.
 */
[[nodiscard]] std::uint64_t
merge_index_files(const std::filesystem::path &output,
                  const std::vector<SegmentSource> &sources);

} // namespace snowseek::storage::detail
