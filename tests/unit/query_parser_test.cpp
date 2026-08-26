/**
 * @file query_parser_test.cpp
 * @brief Verifies query parsing precedence, syntax, and validation behavior.
 */

#include "query/query_parser.hpp"

#include "test_support.hpp"

#include <stdexcept>
#include <string>

namespace {

/** @brief Verifies NOT, AND, and OR precedence without parentheses. */
void applies_operator_precedence() {
        const auto query = snowseek::query::parse_query(
                "alpha OR beta AND NOT gamma");
        snowseek::test::require(
                query->kind == snowseek::query::QueryNodeKind::disjunction,
                "OR should be the root operator");
        snowseek::test::require(
                query->right->kind ==
                        snowseek::query::QueryNodeKind::conjunction,
                "AND should bind more tightly than OR");
        snowseek::test::require(
                query->right->right->kind ==
                        snowseek::query::QueryNodeKind::negation,
                "NOT should bind to the following operand");
}

/** @brief Verifies parentheses override ordinary precedence. */
void honors_parentheses() {
        const auto query =
                snowseek::query::parse_query("(alpha OR beta) AND gamma");
        snowseek::test::require(
                query->kind == snowseek::query::QueryNodeKind::conjunction,
                "parenthesized OR should become the left AND operand");
        snowseek::test::require(
                query->left->kind ==
                        snowseek::query::QueryNodeKind::disjunction,
                "parenthesized group should retain its operator");
}

/** @brief Verifies phrase normalization and escaped quote handling. */
void parses_phrases_and_escapes() {
        const auto query =
                snowseek::query::parse_query("\"Alpha \\\"Beta\\\"\"");
        snowseek::test::require(
                query->kind == snowseek::query::QueryNodeKind::phrase,
                "quoted input should produce a phrase");
        snowseek::test::require_equal(
                query->terms,
                std::vector<std::string>({"alpha", "beta"}),
                "phrase terms should use index normalization");
}

/** @brief Verifies quoted and unquoted filter values are retained. */
void parses_filters() {
        const auto query = snowseek::query::parse_query(
                "path:\"src/my file.cpp\" OR extension:.CPP");
        snowseek::test::require(
                query->left->kind ==
                        snowseek::query::QueryNodeKind::path_filter,
                "path prefix should produce a path filter");
        snowseek::test::require_equal(query->left->value,
                                      std::string("src/my file.cpp"),
                                      "quoted path should be decoded");
        snowseek::test::require(
                query->right->kind ==
                        snowseek::query::QueryNodeKind::extension_filter,
                "extension prefix should produce an extension filter");
}

/** @brief Verifies malformed or implicit syntax is rejected with offsets. */
void rejects_invalid_syntax() {
        snowseek::test::require_throws<std::invalid_argument>(
                [] { static_cast<void>(snowseek::query::parse_query("a b")); },
                "adjacent terms should require explicit AND");
        snowseek::test::require_throws<std::invalid_argument>(
                [] { static_cast<void>(snowseek::query::parse_query("a AND")); },
                "missing right operand should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] { static_cast<void>(snowseek::query::parse_query("(a OR b")); },
                "missing closing parenthesis should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] { static_cast<void>(snowseek::query::parse_query("\"a")); },
                "unterminated quote should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] { static_cast<void>(snowseek::query::parse_query("path:")); },
                "empty filter should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(
                                snowseek::query::parse_query("extension:."));
                },
                "extension filter should require a name");
}

/** @brief Verifies expression byte and AST depth limits. */
void enforces_complexity_limits() {
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(snowseek::query::parse_query(
                                std::string(
                                        snowseek::query::
                                                        kMaxQueryExpressionLength +
                                                1,
                                        'a')));
                },
                "oversized expression should fail before tokenization");

        std::string deep;
        for (std::size_t index = 0;
             index < snowseek::query::kMaxQueryDepth; ++index) {
                deep.append("NOT ");
        }
        deep.append("term");
        snowseek::test::require_throws<std::invalid_argument>(
                [&deep] {
                        static_cast<void>(snowseek::query::parse_query(deep));
                },
                "overly deep AST should fail");

        std::string parentheses(snowseek::query::kMaxQueryDepth + 1, '(');
        parentheses.append("term");
        parentheses.append(snowseek::query::kMaxQueryDepth + 1, ')');
        snowseek::test::require_throws<std::invalid_argument>(
                [&parentheses] {
                        static_cast<void>(
                                snowseek::query::parse_query(parentheses));
                },
                "overly nested parentheses should fail during parsing");
}

} // namespace

/** @brief Runs the query-parser unit-test suite. */
int main() {
        return snowseek::test::run({
                {"applies operator precedence", applies_operator_precedence},
                {"honors parentheses", honors_parentheses},
                {"parses phrases and escapes", parses_phrases_and_escapes},
                {"parses filters", parses_filters},
                {"rejects invalid syntax", rejects_invalid_syntax},
                {"enforces complexity limits", enforces_complexity_limits},
        });
}
