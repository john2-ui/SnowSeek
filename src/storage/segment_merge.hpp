#pragma once

#include "snowseek/storage/index_file.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace snowseek::storage::detail {

struct SegmentSource {
        std::filesystem::path path;
        IndexFileStats stats;
};

/**
 * @brief Merges ordered temporary v1 Segments into one byte-compatible file.
 * @param output Candidate output path whose parent is used for spool files.
 * @param sources Nonempty Segments in global document order.
 * @return Estimated bounded merge workspace bytes.
 * @throws std::runtime_error If an input is malformed, a v1 limit is exceeded,
 * or temporary/output I/O fails.
 */
[[nodiscard]] std::uint64_t
merge_index_files(const std::filesystem::path &output,
                  const std::vector<SegmentSource> &sources);

} // namespace snowseek::storage::detail
