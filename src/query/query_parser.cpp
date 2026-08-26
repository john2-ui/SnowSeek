/**
 * @file query_parser.cpp
 * @brief Tokenizes and parses Boolean query expressions into syntax trees.
 */

#include "query/query_parser.hpp"

#include "analysis/tokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace snowseek::query {
namespace {

enum class TokenKind {
        word,
        phrase,
        path_filter,
        extension_filter,
        conjunction,
        disjunction,
        negation,
        left_parenthesis,
        right_parenthesis,
        end,
};

struct LexToken {
        TokenKind kind{}; ///< Lexical category used by the parser.
        std::string text; ///< Decoded token payload when applicable.
        std::size_t offset{}; ///< Starting byte offset in the expression.
};

/**
 * @brief Tests an ASCII prefix without altering the retained filter value.
 * @param text Candidate text.
 * @param prefix Prefix matched without ASCII case.
 * @return True when text begins with prefix.
 */
[[nodiscard]] bool starts_with_ascii_case_insensitive(
        std::string_view text, std::string_view prefix) {
        if (text.size() < prefix.size()) {
                return false;
        }
        for (std::size_t index = 0; index < prefix.size(); ++index) {
                const auto left = static_cast<unsigned char>(text[index]);
                const auto right = static_cast<unsigned char>(prefix[index]);
                if (std::tolower(left) != std::tolower(right)) {
                        return false;
                }
        }
        return true;
}

/**
 * @brief Produces a syntax error annotated with a source byte offset.
 * @param offset Zero-based byte offset in the expression.
 * @param message Human-readable failure reason.
 * @return Invalid-argument exception ready to throw.
 */
[[nodiscard]] std::invalid_argument syntax_error(std::size_t offset,
                                                 std::string_view message) {
        return std::invalid_argument("query syntax error at byte " +
                                     std::to_string(offset) + ": " +
                                     std::string(message));
}

class Lexer {
      public:
        /**
         * @brief Binds the lexer to an expression retained by the caller.
         * @param input Query bytes that remain valid through parsing.
         */
        explicit Lexer(std::string_view input) : input_(input) {}

        /**
         * @brief Returns and consumes the next lexical token.
         * @return Token with its original byte offset.
         * @throws std::invalid_argument If quoting or escaping is malformed.
         */
        [[nodiscard]] LexToken next() {
                skip_whitespace();
                if (position_ == input_.size()) {
                        return LexToken{TokenKind::end, {}, position_};
                }

                const std::size_t offset = position_;
                if (input_[position_] == '(') {
                        ++position_;
                        return LexToken{TokenKind::left_parenthesis, {}, offset};
                }
                if (input_[position_] == ')') {
                        ++position_;
                        return LexToken{TokenKind::right_parenthesis, {}, offset};
                }
                if (input_[position_] == '"') {
                        ++position_;
                        return LexToken{TokenKind::phrase,
                                        read_quoted_value(offset), offset};
                }

                return read_word(offset);
        }

      private:
        /** @brief Advances past ASCII whitespace between tokens. */
        void skip_whitespace() {
                while (position_ < input_.size() &&
                       std::isspace(static_cast<unsigned char>(
                               input_[position_])) != 0) {
                        ++position_;
                }
        }

        /**
         * @brief Decodes a quoted value accepting escaped quote and backslash.
         * @param quote_offset Offset used for unterminated-value diagnostics.
         * @return Decoded value without surrounding quotes.
         * @throws std::invalid_argument If the value or escape is incomplete.
         */
        [[nodiscard]] std::string read_quoted_value(std::size_t quote_offset) {
                std::string value;
                while (position_ < input_.size()) {
                        const char character = input_[position_++];
                        if (character == '"') {
                                return value;
                        }
                        if (character == '\\') {
                                if (position_ == input_.size()) {
                                        throw syntax_error(
                                                quote_offset,
                                                "unterminated escape sequence");
                                }
                                const char escaped = input_[position_++];
                                if (escaped != '"' && escaped != '\\') {
                                        throw syntax_error(
                                                position_ - 2,
                                                "only quote and backslash may be escaped");
                                }
                                value.push_back(escaped);
                                continue;
                        }
                        value.push_back(character);
                }
                throw syntax_error(quote_offset, "unterminated quoted value");
        }

        /**
         * @brief Reads an operator, unquoted filter, or ordinary term.
         * @param offset Starting byte offset of the token.
         * @return Classified token preserving non-keyword text.
         */
        [[nodiscard]] LexToken read_word(std::size_t offset) {
                constexpr std::string_view path_prefix = "path:";
                constexpr std::string_view extension_prefix = "extension:";
                TokenKind filter_kind = TokenKind::end;
                std::size_t prefix_size = 0;
                if (starts_with_ascii_case_insensitive(input_.substr(position_),
                                                       path_prefix)) {
                        filter_kind = TokenKind::path_filter;
                        prefix_size = path_prefix.size();
                } else if (starts_with_ascii_case_insensitive(
                                   input_.substr(position_), extension_prefix)) {
                        filter_kind = TokenKind::extension_filter;
                        prefix_size = extension_prefix.size();
                }

                if (filter_kind != TokenKind::end) {
                        position_ += prefix_size;
                        if (position_ < input_.size() &&
                            input_[position_] == '"') {
                                const auto quote_offset = position_++;
                                auto value = read_quoted_value(quote_offset);
                                if (position_ < input_.size() &&
                                    input_[position_] != ')' &&
                                    std::isspace(static_cast<unsigned char>(
                                            input_[position_])) == 0) {
                                        throw syntax_error(
                                                position_,
                                                "unexpected byte after filter value");
                                }
                                return LexToken{filter_kind, std::move(value),
                                                offset};
                        }
                }

                while (position_ < input_.size() &&
                       std::isspace(static_cast<unsigned char>(
                               input_[position_])) == 0 &&
                       input_[position_] != '(' && input_[position_] != ')') {
                        if (input_[position_] == '"') {
                                throw syntax_error(position_,
                                                   "unexpected quote in token");
                        }
                        ++position_;
                }
                const std::string_view raw =
                        input_.substr(offset, position_ - offset);
                if (equals_keyword(raw, "AND")) {
                        return LexToken{TokenKind::conjunction, {}, offset};
                }
                if (equals_keyword(raw, "OR")) {
                        return LexToken{TokenKind::disjunction, {}, offset};
                }
                if (equals_keyword(raw, "NOT")) {
                        return LexToken{TokenKind::negation, {}, offset};
                }
                if (filter_kind != TokenKind::end) {
                        return LexToken{filter_kind,
                                        std::string(raw.substr(prefix_size)),
                                        offset};
                }
                return LexToken{TokenKind::word, std::string(raw), offset};
        }

        /**
         * @brief Compares one token with an ASCII operator keyword.
         * @param text Candidate token.
         * @param keyword Uppercase keyword.
         * @return True when both strings match without ASCII case.
         */
        [[nodiscard]] static bool equals_keyword(std::string_view text,
                                                 std::string_view keyword) {
                return text.size() == keyword.size() &&
                       starts_with_ascii_case_insensitive(text, keyword);
        }

        std::string_view input_; ///< Non-owning expression bytes.
        std::size_t position_{}; ///< Offset of the next unread byte.
};

/**
 * @brief Creates a leaf AST node retaining one scalar value.
 * @param kind Leaf node kind.
 * @param value Normalized term or filter value.
 * @return Owning node.
 */
[[nodiscard]] std::unique_ptr<QueryNode> make_value_node(QueryNodeKind kind,
                                                        std::string value) {
        auto node = std::make_unique<QueryNode>();
        node->kind = kind;
        node->value = std::move(value);
        return node;
}

/**
 * @brief Creates a Boolean AST node owning two operands.
 * @param kind Conjunction or disjunction kind.
 * @param left Left operand.
 * @param right Right operand.
 * @return Owning binary node.
 */
[[nodiscard]] std::unique_ptr<QueryNode>
make_binary_node(QueryNodeKind kind, std::unique_ptr<QueryNode> left,
                 std::unique_ptr<QueryNode> right) {
        auto node = std::make_unique<QueryNode>();
        node->kind = kind;
        node->left = std::move(left);
        node->right = std::move(right);
        return node;
}

/**
 * @brief Computes AST depth to bound all later recursive walks.
 * @param node Root of the tree to measure.
 * @return Maximum node count from root to leaf.
 */
[[nodiscard]] std::size_t expression_depth(const QueryNode &node) {
        const std::size_t left =
                node.left == nullptr ? 0 : expression_depth(*node.left);
        const std::size_t right =
                node.right == nullptr ? 0 : expression_depth(*node.right);
        return 1 + std::max(left, right);
}

class Parser {
      public:
        /**
         * @brief Creates a parser and primes its lookahead token.
         * @param expression Complete query expression.
         */
        explicit Parser(std::string_view expression)
            : lexer_(expression), current_(lexer_.next()) {}

        /**
         * @brief Parses the complete expression and enforces AST depth.
         * @return Owning normalized query tree.
         * @throws std::invalid_argument If syntax or depth is invalid.
         */
        [[nodiscard]] std::unique_ptr<QueryNode> parse() {
                if (current_.kind == TokenKind::end) {
                        throw syntax_error(0, "expression is empty");
                }
                auto result = parse_disjunction();
                if (current_.kind != TokenKind::end) {
                        throw syntax_error(current_.offset,
                                           "unexpected trailing token");
                }
                if (expression_depth(*result) > kMaxQueryDepth) {
                        throw std::invalid_argument(
                                "query expression exceeds maximum depth");
                }
                return result;
        }

      private:
        /** @brief Parses left-associative OR expressions. */
        [[nodiscard]] std::unique_ptr<QueryNode> parse_disjunction() {
                auto left = parse_conjunction();
                while (current_.kind == TokenKind::disjunction) {
                        advance();
                        left = make_binary_node(QueryNodeKind::disjunction,
                                                std::move(left),
                                                parse_conjunction());
                }
                return left;
        }

        /** @brief Parses left-associative AND expressions. */
        [[nodiscard]] std::unique_ptr<QueryNode> parse_conjunction() {
                auto left = parse_unary();
                while (current_.kind == TokenKind::conjunction) {
                        advance();
                        left = make_binary_node(QueryNodeKind::conjunction,
                                                std::move(left), parse_unary());
                }
                return left;
        }

        /** @brief Parses prefix NOT before primary expressions. */
        [[nodiscard]] std::unique_ptr<QueryNode> parse_unary() {
                if (current_.kind != TokenKind::negation) {
                        return parse_primary();
                }
                if (nesting_depth_ >= kMaxQueryDepth) {
                        throw std::invalid_argument(
                                "query expression exceeds maximum depth");
                }
                ++nesting_depth_;
                advance();
                auto node = std::make_unique<QueryNode>();
                node->kind = QueryNodeKind::negation;
                node->left = parse_unary();
                --nesting_depth_;
                return node;
        }

        /** @brief Parses terms, phrases, filters, and parenthesized groups. */
        [[nodiscard]] std::unique_ptr<QueryNode> parse_primary() {
                if (current_.kind == TokenKind::left_parenthesis) {
                        if (nesting_depth_ >= kMaxQueryDepth) {
                                throw std::invalid_argument(
                                        "query expression exceeds maximum depth");
                        }
                        const auto offset = current_.offset;
                        ++nesting_depth_;
                        advance();
                        auto node = parse_disjunction();
                        if (current_.kind != TokenKind::right_parenthesis) {
                                throw syntax_error(offset,
                                                   "missing closing parenthesis");
                        }
                        advance();
                        --nesting_depth_;
                        return node;
                }
                if (current_.kind == TokenKind::word) {
                        auto node = make_value_node(
                                QueryNodeKind::term,
                                normalize_term(current_.text, current_.offset));
                        advance();
                        return node;
                }
                if (current_.kind == TokenKind::phrase) {
                        auto node = std::make_unique<QueryNode>();
                        node->kind = QueryNodeKind::phrase;
                        node->terms = tokenizer_.tokenize(current_.text);
                        if (node->terms.empty()) {
                                throw syntax_error(current_.offset,
                                                   "phrase has no searchable terms");
                        }
                        advance();
                        return node;
                }
                if (current_.kind == TokenKind::path_filter ||
                    current_.kind == TokenKind::extension_filter) {
                        if (current_.text.empty()) {
                                throw syntax_error(current_.offset,
                                                   "filter value is empty");
                        }
                        const auto kind =
                                current_.kind == TokenKind::path_filter
                                        ? QueryNodeKind::path_filter
                                        : QueryNodeKind::extension_filter;
                        if (kind == QueryNodeKind::extension_filter &&
                            (current_.text == "." ||
                             current_.text.find_first_of("/\\") !=
                                     std::string::npos)) {
                                throw syntax_error(
                                        current_.offset,
                                        "extension filter must name one extension");
                        }
                        auto node = make_value_node(kind, current_.text);
                        advance();
                        return node;
                }
                throw syntax_error(current_.offset, "expected query operand");
        }

        /**
         * @brief Normalizes one term and rejects inputs split by tokenization.
         * @param raw Unquoted query token.
         * @param offset Byte offset used for diagnostics.
         * @return Single normalized term.
         */
        [[nodiscard]] std::string normalize_term(std::string_view raw,
                                                 std::size_t offset) const {
                auto terms = tokenizer_.tokenize(raw);
                if (terms.size() != 1) {
                        throw syntax_error(offset,
                                           "term must produce exactly one token");
                }
                return std::move(terms.front());
        }

        /** @brief Consumes the current lookahead token. */
        void advance() { current_ = lexer_.next(); }

        Lexer lexer_; ///< Produces source-ordered lookahead tokens.
        LexToken current_; ///< Current unconsumed lookahead token.
        analysis::Tokenizer tokenizer_; ///< Normalizes searchable text.
        std::size_t nesting_depth_{}; ///< Active unary and group nesting.
};

} // namespace

std::unique_ptr<QueryNode> parse_query(std::string_view expression) {
        if (expression.size() > kMaxQueryExpressionLength) {
                throw std::invalid_argument(
                        "query expression exceeds maximum length");
        }
        return Parser(expression).parse();
}

} // namespace snowseek::query
