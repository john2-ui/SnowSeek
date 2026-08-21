#pragma once

#include "snowseek/analysis/tokenizer.hpp"
#include "snowseek/document/document_store.hpp"
#include "snowseek/document/text_reader.hpp"
#include "snowseek/filesystem/scanner.hpp"
#include "snowseek/index/index.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace snowseek::index {

struct InMemoryBuildOptions {
        filesystem::ScanOptions scan_options;
        document::TextReadOptions read_options;
        analysis::TokenizerOptions tokenizer_options;
};

struct BuildError {
        std::filesystem::path path;
        std::string message;
};

struct InMemoryBuildStats {
        std::uint64_t scanned_files{};
        std::uint64_t indexed_files{};
        std::uint64_t failed_files{};
        std::uint64_t indexed_bytes{};
        std::uint64_t token_count{};
};

struct InMemoryBuildResult {
        document::DocumentStore documents;
        InMemoryIndex index;
        std::vector<filesystem::ScanError> scan_errors;
        std::vector<BuildError> document_errors;
        InMemoryBuildStats stats;
};

class InMemoryIndexBuilder {
      public:
        /**
         * @brief Creates a builder and validates its reader and tokenizer
         * configuration.
         * @param options Options controlling file discovery, text reading, and
         * tokenization.
         * @throws std::invalid_argument If a reader or tokenizer option is
         * invalid.
         */
        explicit InMemoryIndexBuilder(InMemoryBuildOptions options = {});

        /**
         * @brief Builds a document table and positional inverted index from a
         * source tree.
         * @param source Root directory scanned for candidate documents.
         * @return The successfully indexed documents, postings, diagnostics,
         * and aggregate statistics. Individual scan and document failures are
         * reported in the result and do not stop later candidates.
         * @throws std::overflow_error If an aggregate statistic or index
         * identifier exceeds its supported range.
         */
        [[nodiscard]] InMemoryBuildResult
        build(const std::filesystem::path &source) const;

      private:
        InMemoryBuildOptions options_;
};

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
