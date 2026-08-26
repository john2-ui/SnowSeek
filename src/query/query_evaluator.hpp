#pragma once

#include "query/query_parser.hpp"
#include "document/document.hpp"

#include <vector>

namespace snowseek::document {
class DocumentStore;
}

namespace snowseek::index {
class InMemoryIndex;
}

namespace snowseek::query {

using DocumentIds = std::vector<document::DocumentId>;

/**
 * @brief Evaluates one parsed Boolean query against an immutable corpus.
 * @param query Normalized query syntax tree.
 * @param documents Contiguous visible document table.
 * @param index Visible positional inverted index.
 * @return Matching document identifiers in ascending order.
 * @throws std::invalid_argument If a phrase requires unavailable positions.
 */
[[nodiscard]] DocumentIds
evaluate_query(const QueryNode &query,
               const document::DocumentStore &documents,
               const index::InMemoryIndex &index);

} // namespace snowseek::query
