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
        const auto query =
                snowseek::query::parse_query("alpha OR beta AND NOT gamma");
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

/** @brief Verifies every adjacent operand form implies conjunction. */
void parses_implicit_conjunctions() {
        const auto precedence =
                snowseek::query::parse_query("alpha OR beta gamma");
        snowseek::test::require(
                precedence->kind ==
                                snowseek::query::QueryNodeKind::disjunction &&
                        precedence->right->kind ==
                                snowseek::query::QueryNodeKind::conjunction,
                "implicit AND should retain explicit Boolean precedence");

        const auto group =
                snowseek::query::parse_query("alpha (beta OR gamma)");
        snowseek::test::require(
                group->kind == snowseek::query::QueryNodeKind::conjunction &&
                        group->right->kind ==
                                snowseek::query::QueryNodeKind::disjunction,
                "a neighboring group should imply AND");

        const auto negation = snowseek::query::parse_query("alpha NOT beta");
        snowseek::test::require(
                negation->kind == snowseek::query::QueryNodeKind::conjunction &&
                        negation->right->kind ==
                                snowseek::query::QueryNodeKind::negation,
                "a neighboring NOT should imply AND");
}

/** @brief Verifies phrase normalization and escaped quote handling. */
void parses_phrases_and_escapes() {
        const auto query =
                snowseek::query::parse_query("\"Alpha \\\"Beta\\\"\"");
        snowseek::test::require(query->kind ==
                                        snowseek::query::QueryNodeKind::phrase,
                                "quoted input should produce a phrase");
        snowseek::test::require_equal(
                query->terms, std::vector<std::string>({"alpha", "beta"}),
                "phrase terms should use index normalization");
}

/** @brief Verifies trailing prefixes and ordered phrase proximity payloads. */
void parses_prefixes_and_proximity() {
        const auto query =
                snowseek::query::parse_query("Error* \"Alpha Beta\"~3");
        snowseek::test::require(
                query->kind == snowseek::query::QueryNodeKind::conjunction &&
                        query->left->kind ==
                                snowseek::query::QueryNodeKind::prefix,
                "a trailing wildcard should produce a prefix operand");
        snowseek::test::require_equal(query->left->value, std::string("error"),
                                      "prefixes should use term normalization");
        snowseek::test::require(
                query->right->kind == snowseek::query::QueryNodeKind::phrase &&
                        query->right->proximity == 3,
                "phrase suffix should retain its proximity");
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

/** @brief Verifies parsed metadata comparisons and normalized values. */
void parses_metadata_filters() {
        const auto query =
                snowseek::query::parse_query("size:>=1MiB mtime:!=2024-02-29");
        snowseek::test::require(
                query->left->kind ==
                                snowseek::query::QueryNodeKind::size_filter &&
                        query->left->comparison ==
                                snowseek::query::ComparisonOperator::
                                        greater_equal &&
                        query->left->size_bytes == 1048576,
                "size filters should parse IEC comparisons");
        snowseek::test::require(
                query->right->kind ==
                                snowseek::query::QueryNodeKind::mtime_filter &&
                        query->right->comparison ==
                                snowseek::query::ComparisonOperator::
                                        not_equal &&
                        query->right->mtime_day == 19782,
                "mtime filters should parse leap-day UTC ordinals");

        const auto defaults =
                snowseek::query::parse_query("size:0 mtime:1970-01-01");
        snowseek::test::require(
                defaults->left->comparison ==
                                snowseek::query::ComparisonOperator::equal &&
                        defaults->left->size_bytes == 0 &&
                        defaults->right->mtime_day == 0,
                "omitted comparisons should mean equality and allow zero");
}

/** @brief Verifies malformed syntax is rejected with offsets. */
void rejects_invalid_syntax() {
        snowseek::test::require_throws<std::invalid_argument>(
                [] { static_cast<void>(snowseek::query::parse_query("*")); },
                "an empty prefix should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] { static_cast<void>(snowseek::query::parse_query("a*b")); },
                "a middle wildcard should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] { static_cast<void>(snowseek::query::parse_query("a?")); },
                "question-mark wildcard should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(
                                snowseek::query::parse_query("\"a b\"~"));
                },
                "phrase proximity should require a number");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(snowseek::query::parse_query(
                                "\"a b\"~4294967296"));
                },
                "phrase proximity should fit uint32_t");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(
                                snowseek::query::parse_query("a AND"));
                },
                "missing right operand should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(
                                snowseek::query::parse_query("(a OR b"));
                },
                "missing closing parenthesis should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] { static_cast<void>(snowseek::query::parse_query("\"a")); },
                "unterminated quote should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(
                                snowseek::query::parse_query("path:"));
                },
                "empty filter should fail");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(
                                snowseek::query::parse_query("extension:."));
                },
                "extension filter should require a name");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(
                                snowseek::query::parse_query("size:1MB"));
                },
                "size filters should reject non-IEC suffixes");
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<void>(snowseek::query::parse_query(
                                "mtime:2023-02-29"));
                },
                "mtime filters should reject invalid civil dates");
}

/** @brief Verifies expression byte and AST depth limits. */
void enforces_complexity_limits() {
        snowseek::test::require_throws<std::invalid_argument>(
                [] {
                        static_cast<
                                void>(snowseek::query::parse_query(std::string(
                                snowseek::query::kMaxQueryExpressionLength + 1,
                                'a')));
                },
                "oversized expression should fail before tokenization");

        std::string deep;
        for (std::size_t index = 0; index < snowseek::query::kMaxQueryDepth;
             ++index) {
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
                {"parses implicit conjunctions", parses_implicit_conjunctions},
                {"parses phrases and escapes", parses_phrases_and_escapes},
                {"parses prefixes and proximity",
                 parses_prefixes_and_proximity},
                {"parses filters", parses_filters},
                {"parses metadata filters", parses_metadata_filters},
                {"rejects invalid syntax", rejects_invalid_syntax},
                {"enforces complexity limits", enforces_complexity_limits},
        });
}
