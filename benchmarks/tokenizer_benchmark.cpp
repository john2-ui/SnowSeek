#include "snowseek/analysis/tokenizer.hpp"

#include <chrono>
#include <iostream>
#include <string>

int main() {
        const std::string input(1024 * 1024, 'a');
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
