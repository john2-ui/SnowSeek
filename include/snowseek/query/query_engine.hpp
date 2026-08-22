#pragma once

#include "snowseek/analysis/tokenizer.hpp"
#include "snowseek/document/document_store.hpp"
#include "snowseek/index/index.hpp"
#include "snowseek/storage/index_file.hpp"

#include <cstddef>
#include <cstdint>
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
        storage::LoadedIndex loaded_;
};

struct InMemoryTermMatch {
        document::DocumentId document_id{};
        std::filesystem::path path;
        std::uint32_t term_frequency{};
        std::vector<index::Position> positions;
};

struct InMemoryDocumentMatch {
        document::DocumentId document_id{};
        std::filesystem::path path;
};

class InMemoryQueryEngine {
      public:
        /**
         * @brief Binds a query engine to an immutable document table and index.
         * @param documents Document metadata referenced by index DocumentIds.
         * @param index Positional inverted index to search.
         * @param tokenizer_options Normalization and query-token length rules.
         * @throws std::invalid_argument If tokenizer_options is invalid.
         * @note documents and index must outlive this query engine.
         */
        InMemoryQueryEngine(const document::DocumentStore &documents,
                            const index::InMemoryIndex &index,
                            analysis::TokenizerOptions tokenizer_options = {});

        /**
         * @brief Finds documents containing one normalized query term.
         * @param term Input that must normalize to exactly one token.
         * @return Matches in ascending DocumentId order, including copied
         * positions and term frequency; returns an empty vector when absent.
         * @throws std::invalid_argument If term produces zero or multiple
         * tokens.
         * @throws std::length_error If term exceeds the tokenizer limit.
         * @throws std::out_of_range If the index references an unknown
         * document.
         * @throws std::overflow_error If a posting frequency is unsupported.
         */
        [[nodiscard]] std::vector<InMemoryTermMatch>
        search_term(std::string_view term) const;

        /**
         * @brief Finds documents containing both normalized query terms.
         * @param left First input that must normalize to exactly one token.
         * @param right Second input that must normalize to exactly one token.
         * @return Common documents in ascending DocumentId order, or an empty
         * vector when either term is absent.
         * @throws std::invalid_argument If either input produces zero or
         * multiple tokens.
         * @throws std::length_error If either input exceeds the tokenizer
         * limit.
         * @throws std::out_of_range If the index references an unknown
         * document.
         */
        [[nodiscard]] std::vector<InMemoryDocumentMatch>
        search_and(std::string_view left, std::string_view right) const;

      private:
        const document::DocumentStore &documents_;
        const index::InMemoryIndex &index_;
        analysis::Tokenizer tokenizer_;
};

} // namespace snowseek::query
