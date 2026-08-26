/**
 * @file search.hpp
 * @brief Declares the public immutable-index search API and result types.
 */

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
        std::string term; ///< Normalized query term.
        std::uint32_t term_frequency{}; ///< Occurrences in this document.
        std::uint32_t document_frequency{}; ///< Visible containing documents.
        double score{}; ///< BM25 contribution to the hit score.
};

/** Source text attached to a search hit when it can be read. */
struct SourceSnippet {
        std::size_t line{}; ///< One-based source line number.
        std::string text; ///< Matching line truncated for presentation.
};

/** One ranked document returned by a search. */
struct SearchHit {
        std::filesystem::path path; ///< Indexed source-relative path.
        double score{};             ///< Total BM25 relevance score.
        std::optional<SourceSnippet> snippet; ///< Optional matching source line.
        std::vector<ScoreDetail> explanation; ///< Optional per-term scoring.
};

/** Options controlling result count and optional presentation data. */
struct SearchOptions {
        std::size_t top_k = 20; ///< Maximum returned hits, at most kMaxTopK.
        std::filesystem::path source_root; ///< Empty disables source snippets.
        bool explain = false; ///< Whether hits include per-term contributions.
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
        std::unique_ptr<Impl> impl_; ///< Owned immutable loaded-index state.
};

} // namespace snowseek
