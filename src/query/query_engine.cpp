#include "snowseek/query/query_engine.hpp"

#include <stdexcept>

namespace snowseek::query {

QueryEngine::QueryEngine(std::filesystem::path index_directory)
    : index_directory_(std::move(index_directory)) {}

std::vector<SearchResult> QueryEngine::search(std::string_view expression,
                                              std::size_t top_k) const {
        if (expression.empty()) {
                throw std::invalid_argument(
                        "query expression must not be empty");
        }
        if (top_k == 0) {
                return {};
        }
        // M3: parser, posting reader, planner and ranker will be connected
        // here.
        return {};
}

} // namespace snowseek::query
