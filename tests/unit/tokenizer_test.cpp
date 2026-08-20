#include "snowseek/analysis/tokenizer.hpp"

#include "test_support.hpp"

#include <string>
#include <vector>

namespace {

void tokenizes_ascii_words_and_identifiers() {
        const snowseek::analysis::Tokenizer tokenizer;
        const std::vector<std::string> expected{"timeout", "retry_policy",
                                                "http2"};
        snowseek::test::require_equal(
                tokenizer.tokenize("Timeout retry_policy HTTP2"), expected,
                "ASCII words should be normalized and tokenized");
}

void ignores_delimiters_and_empty_input() {
        const snowseek::analysis::Tokenizer tokenizer;
        snowseek::test::require(tokenizer.tokenize("").empty(),
                                "empty input should produce no tokens");
        snowseek::test::require(
                tokenizer.tokenize(" \t,.-\n").empty(),
                "delimiter-only input should produce no tokens");
}

void handles_tokens_at_input_boundaries() {
        const snowseek::analysis::Tokenizer tokenizer;
        const std::vector<std::string> expected{"first", "last"};
        snowseek::test::require_equal(tokenizer.tokenize("first/last"),
                                      expected,
                                      "boundary tokens should not be dropped");
}

} // namespace

int main() {
        return snowseek::test::run({
                {"tokenizes ASCII words and identifiers",
                 tokenizes_ascii_words_and_identifiers},
                {"ignores delimiters and empty input",
                 ignores_delimiters_and_empty_input},
                {"handles tokens at input boundaries",
                 handles_tokens_at_input_boundaries},
        });
}
