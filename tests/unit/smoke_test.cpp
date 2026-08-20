#include "snowseek/analysis/tokenizer.hpp"
#include "snowseek/ranking/bm25.hpp"
#include "snowseek/storage/index_header.hpp"

#include <cassert>
#include <sstream>

int main() {
        const snowseek::analysis::Tokenizer tokenizer;
        const auto tokens = tokenizer.tokenize("Timeout retry_policy");
        assert(tokens.size() == 2);
        assert(tokens[0] == "timeout");
        assert(tokens[1] == "retry_policy");
        assert(snowseek::ranking::bm25(2, 3, 100, 10, 80.0) > 0.0);

        std::stringstream stream(std::ios::in | std::ios::out |
                                 std::ios::binary);
        const snowseek::storage::IndexHeader expected{1, 7};
        snowseek::storage::write_header(stream, expected);
        stream.seekg(0);
        const auto actual = snowseek::storage::read_header(stream);
        assert(actual.version == expected.version);
        assert(actual.feature_flags == expected.feature_flags);
}
