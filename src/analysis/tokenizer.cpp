#include "snowseek/analysis/tokenizer.hpp"

#include <cctype>

namespace snowseek::analysis {

std::vector<std::string> Tokenizer::tokenize(std::string_view text) const {
        std::vector<std::string> tokens;
        std::string current;

        for (const unsigned char character : text) {
                if (std::isalnum(character) != 0 || character == '_') {
                        current.push_back(
                                static_cast<char>(std::tolower(character)));
                } else if (!current.empty()) {
                        tokens.push_back(std::move(current));
                        current.clear();
                }
        }
        if (!current.empty()) {
                tokens.push_back(std::move(current));
        }
        return tokens;
}

} // namespace snowseek::analysis
