#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace snowseek::analysis {

class Tokenizer {
public:
    [[nodiscard]] std::vector<std::string> tokenize(std::string_view text) const;
};

}  // namespace snowseek::analysis

