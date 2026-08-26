/**
 * @file tokenizer.cpp
 * @brief Implements streaming token normalization and position tracking.
 */

#include "analysis/tokenizer.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace snowseek::analysis {
namespace {

/**
 * @brief Tests whether a byte may participate in an ASCII token.
 * @param character Byte to classify.
 * @return True for ASCII letters, digits, and underscore.
 */
[[nodiscard]] bool is_ascii_token_character(unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_';
}

/**
 * @brief Lowercases an ASCII letter while preserving other token bytes.
 * @param character Token byte to normalize.
 * @return The normalized byte as a char.
 */
[[nodiscard]] char normalize_ascii(unsigned char character) {
        if (character >= 'A' && character <= 'Z') {
                return static_cast<char>(character + ('a' - 'A'));
        }
        return static_cast<char>(character);
}

/**
 * @brief Verifies that a token callback can be invoked.
 * @param consumer Callback to validate.
 * @throws std::invalid_argument If consumer is empty.
 */
void require_consumer(const TokenConsumer &consumer) {
        if (!consumer) {
                throw std::invalid_argument("token consumer must not be empty");
        }
}

} // namespace

TokenizerSession::TokenizerSession(TokenizerOptions options)
    : options_(options) {
        if (options_.max_token_length == 0) {
                throw std::invalid_argument(
                        "maximum token length must be non-zero");
        }
        pending_.reserve(options_.max_token_length);
}

void TokenizerSession::push(std::string_view chunk,
                            const TokenConsumer &consumer) {
        if (finished_) {
                throw std::logic_error(
                        "cannot push data to a finished tokenizer session");
        }
        require_consumer(consumer);

        for (const unsigned char character : chunk) {
                if (is_ascii_token_character(character)) {
                        if (pending_.size() == options_.max_token_length) {
                                throw std::length_error(
                                        "token exceeds maximum length");
                        }
                        pending_.push_back(normalize_ascii(character));
                } else {
                        emit_pending(consumer);
                }
        }
}

void TokenizerSession::finish(const TokenConsumer &consumer) {
        if (finished_) {
                throw std::logic_error(
                        "tokenizer session has already been finished");
        }
        require_consumer(consumer);
        emit_pending(consumer);
        finished_ = true;
}

void TokenizerSession::emit_pending(const TokenConsumer &consumer) {
        if (pending_.empty()) {
                return;
        }
        if (next_position_ > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("token position exceeds uint32_t");
        }

        Token token{std::move(pending_),
                    static_cast<std::uint32_t>(next_position_)};
        pending_.clear();
        pending_.reserve(options_.max_token_length);
        ++next_position_;
        consumer(std::move(token));
}

Tokenizer::Tokenizer(TokenizerOptions options) : options_(options) {
        if (options_.max_token_length == 0) {
                throw std::invalid_argument(
                        "maximum token length must be non-zero");
        }
}

std::vector<std::string> Tokenizer::tokenize(std::string_view text) const {
        std::vector<std::string> tokens;
        for (auto &token : tokenize_with_positions(text)) {
                tokens.push_back(std::move(token.term));
        }
        return tokens;
}

std::vector<Token>
Tokenizer::tokenize_with_positions(std::string_view text) const {
        std::vector<Token> tokens;
        TokenizerSession session(options_);
        const TokenConsumer consumer = [&tokens](Token token) {
                tokens.push_back(std::move(token));
        };
        session.push(text, consumer);
        session.finish(consumer);
        return tokens;
}

} // namespace snowseek::analysis
