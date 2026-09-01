/**
 * @file query_evaluator.hpp
 * @brief Declares Boolean query evaluation over documents and postings.
 */

#pragma once

#include "document/document.hpp"
#include "query/query_parser.hpp"

#include <vector>

namespace snowseek::document {
class DocumentStore;
}

namespace snowseek::index {
class InMemoryIndex;
}

namespace snowseek::query {

using DocumentIds = std::vector<document::DocumentId>;
inline constexpr std::size_t kMaxExpandedPrefixTerms = 256;

struct QueryEvaluation {
        DocumentIds documents; ///< Matching identifiers in ascending order.
        std::vector<std::string>
                positive_terms; ///< Concrete normalized terms used for ranking.
};

/**
 * @brief Evaluates one parsed Boolean query against an immutable corpus.
 * @param query Normalized query syntax tree.
 * @param documents Contiguous visible document table.
 * @param index Visible positional inverted index.
 * @return Matching identifiers and concrete positive terms for presentation.
 * @throws std::invalid_argument If positions are unavailable or prefix
 * expansion exceeds its query-wide limit.
 */
[[nodiscard]] QueryEvaluation
evaluate_query(const QueryNode &query, const document::DocumentStore &documents,
               const index::InMemoryIndex &index);

} // namespace snowseek::query
