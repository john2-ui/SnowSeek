#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek {

/** Maximum number of hits accepted by one search. */
inline constexpr std::size_t kMaxTopK = 1000;

/** Per-term contribution to a document's total relevance score. */
struct ScoreDetail {
        /** Normalized query term. */
        std::string term;
        /** Occurrences of the term in this document. */
        std::uint32_t term_frequency{};
        /** Visible documents containing the term. */
        std::uint32_t document_frequency{};
        /** BM25 contribution added to the hit score. */
        double score{};
};

/** Source text attached to a search hit when it can be read. */
struct SourceSnippet {
        /** One-based source line number. */
        std::size_t line{};
        /** Matching source line, truncated to the presentation limit. */
        std::string text;
};

/** One ranked document returned by a search. */
struct SearchHit {
        /** Source-relative path stored in the index. */
        std::filesystem::path path;
        /** Total BM25 score. */
        double score{};
        /** Source line, absent when not requested or unavailable. */
        std::optional<SourceSnippet> snippet;
        /** Per-term score details, populated only when requested. */
        std::vector<ScoreDetail> explanation;
};

/** Options controlling result count and optional presentation data. */
struct SearchOptions {
        /** Maximum returned hits. */
        std::size_t top_k = 20;
        /** Corpus root used for snippets; empty disables source reads. */
        std::filesystem::path source_root;
        /** Whether each hit includes per-term score details. */
        bool explain = false;
};

/** Searches one immutable index generation loaded from a directory. */
class Searcher {
      public:
        /**
         * @brief Loads the active index generation from a directory.
         * @param index_directory Directory containing a SnowSeek Manifest.
         * @throws std::runtime_error If the index cannot be read or validated.
         */
        explicit Searcher(const std::filesystem::path &index_directory);

        /** @brief Releases the loaded index generation. */
        ~Searcher();

        Searcher(const Searcher &) = delete;
        Searcher &operator=(const Searcher &) = delete;

        /** @brief Transfers ownership of a loaded search index. */
        Searcher(Searcher &&) noexcept;

        /** @brief Replaces this searcher with another loaded search index. */
        Searcher &operator=(Searcher &&) noexcept;

        /**
         * @brief Searches the loaded index for a Boolean expression.
         * @param expression Nonempty query expression.
         * @param options Result limit, optional source root, and explanation
         * behavior.
         * @return Hits ordered by descending BM25 score, then path bytes.
         * @throws std::invalid_argument If the expression or options are
         * invalid.
         */
        [[nodiscard]] std::vector<SearchHit>
        search(std::string_view expression,
               const SearchOptions &options = {}) const;

      private:
        class Impl;
        std::unique_ptr<Impl> impl_;
};

} // namespace snowseek
