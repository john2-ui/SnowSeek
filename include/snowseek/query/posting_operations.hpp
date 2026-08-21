#pragma once

#include "snowseek/document/document.hpp"
#include "snowseek/index/index.hpp"

#include <vector>

namespace snowseek::query {

/**
 * @brief Intersects two posting lists by their document identifiers.
 * @param left Posting list whose DocumentIds are strictly increasing.
 * @param right Posting list whose DocumentIds are strictly increasing.
 * @return Common DocumentIds in strictly increasing order without duplicates.
 */
[[nodiscard]] std::vector<document::DocumentId>
intersect_document_ids(const index::PostingList &left,
                       const index::PostingList &right);

} // namespace snowseek::query
