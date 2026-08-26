#include "analysis/tokenizer.hpp"

#include <chrono>
#include <iostream>
#include <string>

/**
 * @brief Measures one-shot tokenization of approximately one MiB of text.
 * @return Zero after printing bytes, tokens, and elapsed microseconds.
 */
int main() {
        constexpr std::size_t target_size = 1024U * 1024U;
        std::string input;
        input.reserve(target_size);
        while (input.size() < target_size) {
                input.append("retry_policy ");
        }
        input.resize(target_size);

        const snowseek::analysis::Tokenizer tokenizer;
        const auto begin = std::chrono::steady_clock::now();
        const auto tokens = tokenizer.tokenize(input);
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        std::cout << "bytes=" << input.size() << " tokens=" << tokens.size()
                  << " elapsed_us="
                  << std::chrono::duration_cast<std::chrono::microseconds>(
                             elapsed)
                             .count()
                  << '\n';
}
