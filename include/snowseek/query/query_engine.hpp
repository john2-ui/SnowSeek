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
        explicit QueryEngine(std::filesystem::path index_directory);
        [[nodiscard]] std::vector<SearchResult>
        search(std::string_view expression, std::size_t top_k = 20) const;

      private:
        std::filesystem::path index_directory_;
};

} // namespace snowseek::query
