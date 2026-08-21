#pragma once

#include <cstddef>
#include <filesystem>

namespace snowseek::index {

struct BuildOptions {
        std::size_t memory_limit_bytes = 64U * 1024U * 1024U;
        unsigned int threads = 1;
        bool store_positions = true;
};

class IndexBuilder {
      public:
        /**
         * @brief Creates a persistent-index builder configuration.
         * @param options Memory, concurrency, and positional-index settings.
         * @throws std::invalid_argument If memory or thread limits are zero.
         */
        explicit IndexBuilder(BuildOptions options = {});

        /**
         * @brief Prepares an index directory for a source tree.
         * @param source Existing source path to index.
         * @param index_directory Destination directory created when necessary.
         * @throws std::runtime_error If source does not exist.
         * @throws std::filesystem::filesystem_error If destination creation
         * fails.
         */
        void build(const std::filesystem::path &source,
                   const std::filesystem::path &index_directory) const;

      private:
        BuildOptions options_;
};

} // namespace snowseek::index
