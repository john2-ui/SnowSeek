#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::query {

struct SearchResult {
        std::filesystem::path path;
        std::size_t line{};
        double score{};
        std::string snippet;
};

class QueryEngine {
      public:
        /**
         * @brief Creates a query engine bound to an index directory.
         * @param index_directory Directory containing the searchable index.
         */
        explicit QueryEngine(std::filesystem::path index_directory);

        /**
         * @brief Searches the bound index for an expression.
         * @param expression Nonempty query expression.
         * @param top_k Maximum number of results; zero requests no results.
         * @return Ranked results in descending relevance order.
         * @throws std::invalid_argument If expression is empty.
         */
        [[nodiscard]] std::vector<SearchResult>
        search(std::string_view expression, std::size_t top_k = 20) const;

      private:
        std::filesystem::path index_directory_;
};

} // namespace snowseek::query
