#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::query {

inline constexpr std::size_t kMaxQueryExpressionLength = 4096;
inline constexpr std::size_t kMaxQueryDepth = 32;

enum class QueryNodeKind {
        term,
        phrase,
        path_filter,
        extension_filter,
        conjunction,
        disjunction,
        negation,
};

struct QueryNode {
        QueryNodeKind kind{};
        std::string value;
        std::vector<std::string> terms;
        std::unique_ptr<QueryNode> left;
        std::unique_ptr<QueryNode> right;
};

/**
 * @brief Parses and normalizes one query expression.
 * @param expression Boolean expression containing terms, phrases, or filters.
 * @return Owning syntax tree with normalized term and phrase tokens.
 * @throws std::invalid_argument If syntax, filter values, length, or nesting
 * limits are invalid.
 * @throws std::length_error If a normalized token exceeds tokenizer limits.
 */
[[nodiscard]] std::unique_ptr<QueryNode>
parse_query(std::string_view expression);

} // namespace snowseek::query
