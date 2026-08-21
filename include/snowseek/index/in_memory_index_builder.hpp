#pragma once

#include "snowseek/analysis/tokenizer.hpp"
#include "snowseek/document/document_store.hpp"
#include "snowseek/document/text_reader.hpp"
#include "snowseek/filesystem/scanner.hpp"
#include "snowseek/index/in_memory_index.hpp"

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
        explicit InMemoryIndexBuilder(InMemoryBuildOptions options = {});

        [[nodiscard]] InMemoryBuildResult
        build(const std::filesystem::path &source) const;

      private:
        InMemoryBuildOptions options_;
};

} // namespace snowseek::index
