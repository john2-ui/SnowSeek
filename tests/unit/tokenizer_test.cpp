#include "analysis/tokenizer.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/** @brief Verifies ASCII tokenization and lowercase normalization. */
void tokenizes_ascii_words_and_identifiers() {
        const snowseek::analysis::Tokenizer tokenizer;
        const std::vector<std::string> expected{"timeout", "retry_policy",
                                                "http2"};
        snowseek::test::require_equal(
                tokenizer.tokenize("Timeout retry_policy HTTP2"), expected,
                "ASCII words should be normalized and tokenized");
}

/** @brief Verifies that empty and delimiter-only inputs emit no tokens. */
void ignores_delimiters_and_empty_input() {
        const snowseek::analysis::Tokenizer tokenizer;
        snowseek::test::require(tokenizer.tokenize("").empty(),
                                "empty input should produce no tokens");
        snowseek::test::require(
                tokenizer.tokenize(" \t,.-\n").empty(),
                "delimiter-only input should produce no tokens");
}

/** @brief Verifies emission of tokens adjacent to input boundaries. */
void handles_tokens_at_input_boundaries() {
        const snowseek::analysis::Tokenizer tokenizer;
        const std::vector<std::string> expected{"first", "last"};
        snowseek::test::require_equal(tokenizer.tokenize("first/last"),
                                      expected,
                                      "boundary tokens should not be dropped");
}

/** @brief Verifies zero-based, strictly increasing token positions. */
void assigns_strictly_increasing_positions() {
        const snowseek::analysis::Tokenizer tokenizer;
        const auto tokens =
                tokenizer.tokenize_with_positions("First second THIRD");

        snowseek::test::require_equal(tokens.size(), std::size_t{3},
                                      "three positioned tokens are expected");
        snowseek::test::require_equal(tokens[0].term, std::string("first"),
                                      "the first term should be normalized");
        snowseek::test::require_equal(tokens[0].position, std::uint32_t{0},
                                      "positions should start at zero");
        snowseek::test::require_equal(tokens[1].position, std::uint32_t{1},
                                      "the second position should increase");
        snowseek::test::require_equal(tokens[2].position, std::uint32_t{2},
                                      "the third position should increase");
}

/** @brief Verifies preservation of token state across streamed chunks. */
void preserves_tokens_across_chunks() {
        snowseek::analysis::TokenizerSession session;
        std::vector<snowseek::analysis::Token> tokens;
        const snowseek::analysis::TokenConsumer consumer =
                [&tokens](snowseek::analysis::Token token) {
                        tokens.push_back(std::move(token));
                };

        session.push("Retry_", consumer);
        session.push("Policy next ", consumer);
        session.push("Final", consumer);
        session.finish(consumer);

        snowseek::test::require_equal(tokens.size(), std::size_t{3},
                                      "three streamed tokens are expected");
        snowseek::test::require_equal(
                tokens[0].term, std::string("retry_policy"),
                "a token split across chunks should stay intact");
        snowseek::test::require_equal(
                tokens[1].term, std::string("next"),
                "a delimited token should be emitted during push");
        snowseek::test::require_equal(
                tokens[2].term, std::string("final"),
                "finish should emit the final unterminated token");
}

/** @brief Verifies identifier handling and non-ASCII delimiters. */
void handles_identifier_and_non_ascii_boundaries() {
        const snowseek::analysis::Tokenizer tokenizer;
        const std::string input = "camelCase snake_case HTTP2 \xe4\xb8\xad end";
        const std::vector<std::string> expected{"camelcase", "snake_case",
                                                "http2", "end"};

        snowseek::test::require_equal(tokenizer.tokenize(input), expected,
                                      "identifier forms should remain intact "
                                      "and UTF-8 should delimit");
}

/** @brief Verifies acceptance and rejection at the token-length limit. */
void enforces_maximum_token_length() {
        snowseek::analysis::TokenizerOptions options;
        options.max_token_length = 4;
        const snowseek::analysis::Tokenizer tokenizer(options);

        snowseek::test::require_equal(
                tokenizer.tokenize("ABCD"), std::vector<std::string>{"abcd"},
                "a token at the configured limit should be accepted");
        snowseek::test::require_throws<std::length_error>(
                [&tokenizer] {
                        static_cast<void>(tokenizer.tokenize("ABCDE"));
                },
                "an oversized token should be rejected");
}

/** @brief Verifies tokenizer configuration and session lifecycle checks. */
void validates_session_lifecycle() {
        snowseek::analysis::TokenizerOptions options;
        options.max_token_length = 0;
        snowseek::test::require_throws<std::invalid_argument>(
                [&options] {
                        const snowseek::analysis::TokenizerSession session(
                                options);
                        static_cast<void>(session);
                },
                "a zero token limit should be rejected");

        snowseek::analysis::TokenizerSession session;
        const snowseek::analysis::TokenConsumer consumer =
                [](snowseek::analysis::Token) {};
        session.finish(consumer);
        snowseek::test::require_throws<std::logic_error>(
                [&session, &consumer] { session.push("late", consumer); },
                "a finished session should reject additional input");
}

} // namespace

/** @brief Runs the tokenizer unit-test suite. */
int main() {
        return snowseek::test::run({
                {"tokenizes ASCII words and identifiers",
                 tokenizes_ascii_words_and_identifiers},
                {"ignores delimiters and empty input",
                 ignores_delimiters_and_empty_input},
                {"handles tokens at input boundaries",
                 handles_tokens_at_input_boundaries},
                {"assigns strictly increasing positions",
                 assigns_strictly_increasing_positions},
                {"preserves tokens across chunks",
                 preserves_tokens_across_chunks},
                {"handles identifier and non-ASCII boundaries",
                 handles_identifier_and_non_ascii_boundaries},
                {"enforces maximum token length",
                 enforces_maximum_token_length},
                {"validates the session lifecycle",
                 validates_session_lifecycle},
        });
}
