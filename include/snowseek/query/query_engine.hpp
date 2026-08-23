#pragma once

#include "snowseek/storage/index_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::query {

inline constexpr std::size_t kMaxTopK = 1000;

struct ScoreContribution {
        std::string term;
        std::uint32_t term_frequency{};
        std::uint32_t document_frequency{};
        double score{};
};

struct SearchResult {
        std::filesystem::path path;
        std::size_t line{};
        double score{};
        std::string snippet;
        std::vector<ScoreContribution> explanation;
};

struct SearchOptions {
        std::size_t top_k = 20;
        std::filesystem::path source_root;
        bool explain = false;
};

class QueryEngine {
      public:
        /**
         * @brief Creates a query engine bound to an index directory.
         * @param index_directory Directory containing the searchable index.
         */
        explicit QueryEngine(const std::filesystem::path &index_directory);

        /**
         * @brief Searches the bound index for an expression.
         * @param expression Nonempty query expression.
         * @param options Result limit, optional source root, and explanation
         * behavior.
         * @return Results ordered by descending BM25 score with stable path
         * ties.
         * @throws std::invalid_argument If expression, source root, or limits
         * are invalid.
         */
        [[nodiscard]] std::vector<SearchResult>
        search(std::string_view expression,
               const SearchOptions &options = {}) const;

      private:
        storage::LoadedIndex loaded_;
};

} // namespace snowseek::query
