/**
 * @file tokenizer.hpp
 * @brief Declares one-shot and streaming text tokenization interfaces.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::analysis {

struct TokenizerOptions {
        std::size_t max_token_length = 256; ///< Maximum ASCII token bytes.
};

struct Token {
        std::string term;        ///< Lowercase normalized token text.
        std::uint32_t position{}; ///< Zero-based ordinal in the document.
};

using TokenConsumer = std::function<void(Token)>;

class TokenizerSession {
      public:
        /**
         * @brief Creates a streaming tokenizer session.
         * @param options Token length and normalization limits for the session.
         * @throws std::invalid_argument If the maximum token length is zero.
         */
        explicit TokenizerSession(TokenizerOptions options = {});

        /**
         * @brief Consumes a text chunk while preserving tokens across chunks.
         * @param chunk Next contiguous text chunk to tokenize.
         * @param consumer Callback receiving each completed token.
         * @throws std::invalid_argument If consumer is empty.
         * @throws std::logic_error If the session has already been finished.
         * @throws std::length_error If a token exceeds the configured limit.
         */
        void push(std::string_view chunk, const TokenConsumer &consumer);

        /**
         * @brief Emits the final pending token and closes the session.
         * @param consumer Callback receiving the final token, when present.
         * @throws std::invalid_argument If consumer is empty.
         * @throws std::logic_error If the session was already finished.
         */
        void finish(const TokenConsumer &consumer);

      private:
        /**
         * @brief Emits and clears the pending token when one is present.
         * @param consumer Callback receiving ownership of the completed token.
         * @throws std::overflow_error If the next position exceeds uint32_t.
         */
        void emit_pending(const TokenConsumer &consumer);

        TokenizerOptions options_; ///< Token limits fixed for the session.
        std::string pending_;       ///< Normalized token bytes retained until emission.
        std::uint64_t next_position_{}; ///< Ordinal assigned on next emission.
        bool finished_ = false; ///< Whether final input has been consumed.
};

class Tokenizer {
      public:
        /**
         * @brief Creates a one-shot tokenizer with the supplied limits.
         * @param options Token length and normalization limits.
         * @throws std::invalid_argument If the maximum token length is zero.
         */
        explicit Tokenizer(TokenizerOptions options = {});

        /**
         * @brief Tokenizes text into normalized terms without positions.
         * @param text Complete text to tokenize.
         * @return Terms in their source order.
         */
        [[nodiscard]] std::vector<std::string>
        tokenize(std::string_view text) const;

        /**
         * @brief Tokenizes text into normalized terms and ordinal positions.
         * @param text Complete text to tokenize.
         * @return Tokens in source order with zero-based positions.
         */
        [[nodiscard]] std::vector<Token>
        tokenize_with_positions(std::string_view text) const;

      private:
        TokenizerOptions options_; ///< Limits copied into each new session.
};

} // namespace snowseek::analysis
