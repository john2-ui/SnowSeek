/**
 * @file query_parser.cpp
 * @brief Tokenizes and parses Boolean query expressions into syntax trees.
 */

#include "query/query_parser.hpp"

#include "analysis/tokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
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
        size_filter,
        mtime_filter,
        conjunction,
        disjunction,
        negation,
        left_parenthesis,
        right_parenthesis,
        end,
};

struct LexToken {
        TokenKind kind{};          ///< Lexical category used by the parser.
        std::string text;          ///< Decoded token payload when applicable.
        std::size_t offset{};      ///< Starting byte offset in the expression.
        std::uint32_t proximity{}; ///< Phrase proximity suffix, otherwise zero.
};

/**
 * @brief Tests an ASCII prefix without altering the retained filter value.
 * @param text Candidate text.
 * @param prefix Prefix matched without ASCII case.
 * @return True when text begins with prefix.
 */
[[nodiscard]] bool starts_with_ascii_case_insensitive(std::string_view text,
                                                      std::string_view prefix) {
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
                        return LexToken{
                                TokenKind::left_parenthesis, {}, offset};
                }
                if (input_[position_] == ')') {
                        ++position_;
                        return LexToken{
                                TokenKind::right_parenthesis, {}, offset};
                }
                if (input_[position_] == '"') {
                        ++position_;
                        auto token =
                                LexToken{TokenKind::phrase,
                                         read_quoted_value(offset), offset};
                        if (position_ < input_.size() &&
                            input_[position_] == '~') {
                                token.proximity = read_proximity(position_++);
                        }
                        if (position_ < input_.size() &&
                            input_[position_] != ')' &&
                            std::isspace(static_cast<unsigned char>(
                                    input_[position_])) == 0) {
                                throw syntax_error(
                                        position_,
                                        "unexpected byte after phrase");
                        }
                        return token;
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
         * @brief Parses the decimal suffix following a phrase tilde.
         * @param tilde_offset Byte offset of the consumed tilde.
         * @return Extra ordered span permitted for the phrase.
         * @throws std::invalid_argument If the suffix is absent or malformed.
         */
        [[nodiscard]] std::uint32_t read_proximity(std::size_t tilde_offset) {
                const auto begin = input_.data() + position_;
                const auto end = input_.data() + input_.size();
                std::uint32_t value = 0;
                const auto result = std::from_chars(begin, end, value);
                if (result.ptr == begin || result.ec != std::errc{}) {
                        throw syntax_error(tilde_offset,
                                           "invalid phrase proximity");
                }
                position_ =
                        static_cast<std::size_t>(result.ptr - input_.data());
                return value;
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
                                                "only quote and backslash may "
                                                "be escaped");
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
                constexpr std::string_view size_prefix = "size:";
                constexpr std::string_view mtime_prefix = "mtime:";
                TokenKind filter_kind = TokenKind::end;
                std::size_t prefix_size = 0;
                if (starts_with_ascii_case_insensitive(input_.substr(position_),
                                                       path_prefix)) {
                        filter_kind = TokenKind::path_filter;
                        prefix_size = path_prefix.size();
                } else if (starts_with_ascii_case_insensitive(
                                   input_.substr(position_),
                                   extension_prefix)) {
                        filter_kind = TokenKind::extension_filter;
                        prefix_size = extension_prefix.size();
                } else if (starts_with_ascii_case_insensitive(
                                   input_.substr(position_), size_prefix)) {
                        filter_kind = TokenKind::size_filter;
                        prefix_size = size_prefix.size();
                } else if (starts_with_ascii_case_insensitive(
                                   input_.substr(position_), mtime_prefix)) {
                        filter_kind = TokenKind::mtime_filter;
                        prefix_size = mtime_prefix.size();
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
                                                "unexpected byte after filter "
                                                "value");
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

struct ParsedComparison {
        ComparisonOperator comparison{}; ///< Operator selected by the prefix.
        std::string_view operand; ///< Value bytes following the operator.
};

/**
 * @brief Splits an optional comparison operator from a filter value.
 * @param text Complete filter payload after its field prefix.
 * @param offset Source offset used for empty-value diagnostics.
 * @return Parsed operator and nonempty operand view.
 * @throws std::invalid_argument If no operand follows the operator.
 */
[[nodiscard]] ParsedComparison parse_comparison(std::string_view text,
                                                std::size_t offset) {
        ComparisonOperator comparison = ComparisonOperator::equal;
        std::size_t operator_size = 0;
        if (text.starts_with("!=")) {
                comparison = ComparisonOperator::not_equal;
                operator_size = 2;
        } else if (text.starts_with("<=")) {
                comparison = ComparisonOperator::less_equal;
                operator_size = 2;
        } else if (text.starts_with(">=")) {
                comparison = ComparisonOperator::greater_equal;
                operator_size = 2;
        } else if (text.starts_with('=')) {
                operator_size = 1;
        } else if (text.starts_with('<')) {
                comparison = ComparisonOperator::less;
                operator_size = 1;
        } else if (text.starts_with('>')) {
                comparison = ComparisonOperator::greater;
                operator_size = 1;
        }
        const auto operand = text.substr(operator_size);
        if (operand.empty()) {
                throw syntax_error(offset, "filter value is empty");
        }
        return ParsedComparison{comparison, operand};
}

/**
 * @brief Parses a nonnegative byte count with an optional IEC suffix.
 * @param text Decimal bytes followed by B, KiB, MiB, GiB, TiB, or nothing.
 * @param offset Source offset used for diagnostics.
 * @return Converted byte count.
 * @throws std::invalid_argument If the number or suffix is malformed.
 * @throws std::overflow_error If conversion exceeds uint64_t.
 */
[[nodiscard]] std::uint64_t parse_size(std::string_view text,
                                       std::size_t offset) {
        std::uint64_t value = 0;
        const auto result =
                std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec == std::errc::result_out_of_range) {
                throw std::overflow_error("query size exceeds uint64_t");
        }
        if (result.ptr == text.data() || result.ec != std::errc{}) {
                throw syntax_error(offset,
                                   "size must be a nonnegative integer");
        }

        const auto suffix =
                text.substr(static_cast<std::size_t>(result.ptr - text.data()));
        std::uint64_t multiplier = 1;
        if (suffix.empty() || suffix == "B") {
                multiplier = 1;
        } else if (suffix == "KiB") {
                multiplier = 1ULL << 10U;
        } else if (suffix == "MiB") {
                multiplier = 1ULL << 20U;
        } else if (suffix == "GiB") {
                multiplier = 1ULL << 30U;
        } else if (suffix == "TiB") {
                multiplier = 1ULL << 40U;
        } else {
                throw syntax_error(offset, "size has an invalid IEC suffix");
        }
        if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
                throw std::overflow_error("query size exceeds uint64_t");
        }
        return value * multiplier;
}

/**
 * @brief Reports whether a proleptic Gregorian year contains February 29.
 * @param year Positive civil year.
 * @return True when the year is a leap year.
 */
[[nodiscard]] bool is_leap_year(int year) noexcept {
        return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/**
 * @brief Converts a validated Gregorian date to days relative to Unix Epoch.
 * @param year Civil year.
 * @param month One-based civil month.
 * @param day One-based civil day.
 * @return Signed day ordinal where 1970-01-01 is zero.
 */
[[nodiscard]] std::int64_t days_from_civil(int year, unsigned month,
                                           unsigned day) noexcept {
        year -= month <= 2U ? 1 : 0;
        const auto era = year / 400;
        const auto year_of_era = static_cast<unsigned>(year - era * 400);
        const auto shifted_month = static_cast<unsigned>(
                static_cast<int>(month) + (month > 2U ? -3 : 9));
        const auto day_of_year = (153U * shifted_month + 2U) / 5U + day - 1U;
        const auto day_of_era = year_of_era * 365U + year_of_era / 4U -
                                year_of_era / 100U + day_of_year;
        return static_cast<std::int64_t>(era) * 146097 + day_of_era - 719468;
}

/**
 * @brief Parses one strict YYYY-MM-DD UTC civil date.
 * @param text Date bytes to validate.
 * @param offset Source offset used for diagnostics.
 * @return Day ordinal relative to Unix Epoch.
 * @throws std::invalid_argument If syntax or the civil date is invalid.
 */
[[nodiscard]] std::int64_t parse_mtime_day(std::string_view text,
                                           std::size_t offset) {
        const auto digit = [&text](std::size_t index) {
                return text[index] >= '0' && text[index] <= '9';
        };
        if (text.size() != 10 || text[4] != '-' || text[7] != '-' ||
            !digit(0) || !digit(1) || !digit(2) || !digit(3) || !digit(5) ||
            !digit(6) || !digit(8) || !digit(9)) {
                throw syntax_error(offset, "mtime must use YYYY-MM-DD");
        }
        const int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
                         (text[2] - '0') * 10 + (text[3] - '0');
        const unsigned month =
                static_cast<unsigned>((text[5] - '0') * 10 + (text[6] - '0'));
        const unsigned day =
                static_cast<unsigned>((text[8] - '0') * 10 + (text[9] - '0'));
        constexpr unsigned month_days[] = {31, 28, 31, 30, 31, 30,
                                           31, 31, 30, 31, 30, 31};
        if (year == 0 || month == 0 || month > 12) {
                throw syntax_error(offset, "mtime is not a valid civil date");
        }
        const unsigned maximum_day =
                month == 2 && is_leap_year(year) ? 29 : month_days[month - 1];
        if (day == 0 || day > maximum_day) {
                throw syntax_error(offset, "mtime is not a valid civil date");
        }
        return days_from_civil(year, month, day);
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

        /** @brief Parses explicit and adjacent left-associative AND terms. */
        [[nodiscard]] std::unique_ptr<QueryNode> parse_conjunction() {
                auto left = parse_unary();
                while (current_.kind == TokenKind::conjunction ||
                       starts_operand(current_.kind)) {
                        if (current_.kind == TokenKind::conjunction) {
                                advance();
                        }
                        left = make_binary_node(QueryNodeKind::conjunction,
                                                std::move(left), parse_unary());
                }
                return left;
        }

        /**
         * @brief Tests whether lookahead can begin an implicitly joined term.
         * @param kind Lookahead lexical kind.
         * @return True when adjacency represents an implicit AND.
         */
        [[nodiscard]] static bool starts_operand(TokenKind kind) noexcept {
                return kind == TokenKind::word || kind == TokenKind::phrase ||
                       kind == TokenKind::path_filter ||
                       kind == TokenKind::extension_filter ||
                       kind == TokenKind::size_filter ||
                       kind == TokenKind::mtime_filter ||
                       kind == TokenKind::negation ||
                       kind == TokenKind::left_parenthesis;
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
                                        "query expression exceeds maximum "
                                        "depth");
                        }
                        const auto offset = current_.offset;
                        ++nesting_depth_;
                        advance();
                        auto node = parse_disjunction();
                        if (current_.kind != TokenKind::right_parenthesis) {
                                throw syntax_error(
                                        offset, "missing closing parenthesis");
                        }
                        advance();
                        --nesting_depth_;
                        return node;
                }
                if (current_.kind == TokenKind::word) {
                        const auto wildcard =
                                current_.text.find_first_of("*?~^[]{}");
                        QueryNodeKind kind = QueryNodeKind::term;
                        std::string_view raw = current_.text;
                        if (wildcard != std::string::npos) {
                                if (current_.text.size() < 2 ||
                                    wildcard != current_.text.size() - 1 ||
                                    current_.text.back() != '*' ||
                                    current_.text.find_first_of("*?~^[]{}",
                                                                wildcard + 1) !=
                                            std::string::npos) {
                                        throw syntax_error(
                                                current_.offset,
                                                "only one trailing prefix "
                                                "wildcard is supported");
                                }
                                kind = QueryNodeKind::prefix;
                                raw.remove_suffix(1);
                        }
                        auto node = make_value_node(
                                kind, normalize_term(raw, current_.offset));
                        advance();
                        return node;
                }
                if (current_.kind == TokenKind::phrase) {
                        auto node = std::make_unique<QueryNode>();
                        node->kind = QueryNodeKind::phrase;
                        node->terms = tokenizer_.tokenize(current_.text);
                        node->proximity = current_.proximity;
                        if (node->terms.empty()) {
                                throw syntax_error(
                                        current_.offset,
                                        "phrase has no searchable terms");
                        }
                        advance();
                        return node;
                }
                if (current_.kind == TokenKind::path_filter ||
                    current_.kind == TokenKind::extension_filter ||
                    current_.kind == TokenKind::size_filter ||
                    current_.kind == TokenKind::mtime_filter) {
                        if (current_.text.empty()) {
                                throw syntax_error(current_.offset,
                                                   "filter value is empty");
                        }
                        QueryNodeKind kind = QueryNodeKind::path_filter;
                        if (current_.kind == TokenKind::extension_filter) {
                                kind = QueryNodeKind::extension_filter;
                        } else if (current_.kind == TokenKind::size_filter) {
                                kind = QueryNodeKind::size_filter;
                        } else if (current_.kind == TokenKind::mtime_filter) {
                                kind = QueryNodeKind::mtime_filter;
                        }
                        if (kind == QueryNodeKind::extension_filter &&
                            (current_.text == "." ||
                             current_.text.find_first_of("/\\") !=
                                     std::string::npos)) {
                                throw syntax_error(current_.offset,
                                                   "extension filter must name "
                                                   "one extension");
                        }
                        auto node = make_value_node(kind, current_.text);
                        if (kind == QueryNodeKind::size_filter ||
                            kind == QueryNodeKind::mtime_filter) {
                                const auto parsed = parse_comparison(
                                        current_.text, current_.offset);
                                node->comparison = parsed.comparison;
                                if (kind == QueryNodeKind::size_filter) {
                                        node->size_bytes =
                                                parse_size(parsed.operand,
                                                           current_.offset);
                                } else {
                                        node->mtime_day = parse_mtime_day(
                                                parsed.operand,
                                                current_.offset);
                                }
                        }
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
                        throw syntax_error(
                                offset, "term must produce exactly one token");
                }
                return std::move(terms.front());
        }

        /** @brief Consumes the current lookahead token. */
        void advance() { current_ = lexer_.next(); }

        Lexer lexer_;      ///< Produces source-ordered lookahead tokens.
        LexToken current_; ///< Current unconsumed lookahead token.
        analysis::Tokenizer tokenizer_; ///< Normalizes searchable text.
        std::size_t nesting_depth_{};   ///< Active unary and group nesting.
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
