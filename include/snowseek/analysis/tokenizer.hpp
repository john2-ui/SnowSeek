#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::analysis {

struct TokenizerOptions {
        std::size_t max_token_length = 256;
};

struct Token {
        std::string term;
        std::uint32_t position{};
};

using TokenConsumer = std::function<void(Token)>;

class TokenizerSession {
      public:
        explicit TokenizerSession(TokenizerOptions options = {});

        void push(std::string_view chunk, const TokenConsumer &consumer);
        void finish(const TokenConsumer &consumer);

      private:
        void emit_pending(const TokenConsumer &consumer);

        TokenizerOptions options_;
        std::string pending_;
        std::uint64_t next_position_{};
        bool finished_ = false;
};

class Tokenizer {
      public:
        explicit Tokenizer(TokenizerOptions options = {});

        [[nodiscard]] std::vector<std::string>
        tokenize(std::string_view text) const;

        [[nodiscard]] std::vector<Token>
        tokenize_with_positions(std::string_view text) const;

      private:
        TokenizerOptions options_;
};

} // namespace snowseek::analysis
