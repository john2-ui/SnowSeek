/**
 * @file query_parser.hpp
 * @brief Defines the normalized query tree and parsing interface.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::query {

inline constexpr std::size_t kMaxQueryExpressionLength = 4096;
inline constexpr std::size_t kMaxQueryDepth = 32;

enum class QueryNodeKind {
        term,
        prefix,
        phrase,
        path_filter,
        extension_filter,
        size_filter,
        mtime_filter,
        conjunction,
        disjunction,
        negation,
};

/** @brief Comparison applied by numeric and date metadata filters. */
enum class ComparisonOperator {
        equal,
        not_equal,
        less,
        less_equal,
        greater,
        greater_equal,
};

struct QueryNode {
        QueryNodeKind kind{}; ///< Determines which payload fields are active.
        std::string value;    ///< Normalized term or retained filter value.
        std::vector<std::string> terms; ///< Normalized phrase terms in order.
        std::uint32_t proximity{}; ///< Extra ordered phrase span permitted.
        ComparisonOperator comparison{
                ComparisonOperator::equal}; ///< Metadata comparison.
        std::uint64_t size_bytes{}; ///< Parsed size-filter value in bytes.
        std::int64_t mtime_day{};   ///< Parsed UTC civil day relative to Epoch.
        std::unique_ptr<QueryNode>
                left; ///< Unary operand or left binary child.
        std::unique_ptr<QueryNode>
                right; ///< Right binary child, otherwise null.
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
