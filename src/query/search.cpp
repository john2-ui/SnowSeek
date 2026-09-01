/**
 * @file search.cpp
 * @brief Loads, evaluates, ranks, and presents persisted-index searches.
 */

#include "snowseek/search.hpp"

#include "analysis/tokenizer.hpp"
#include "document/document_store.hpp"
#include "document/text_reader.hpp"
#include "index/index.hpp"
#include "query/query_evaluator.hpp"
#include "query/query_parser.hpp"
#include "ranking/bm25.hpp"
#include "storage/index_file.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowseek {
namespace {

struct RankedDocument {
        document::DocumentId document_id{}; ///< Candidate document identifier.
        double score{};                     ///< BM25 relevance score.
        std::string path_key;               ///< Deterministic tie-break key.
};

struct SnippetReady final {};

/**
 * @brief Validates presentation options before executing a query.
 * @param options Search options supplied by the caller.
 * @throws std::invalid_argument If a limit or source root is invalid.
 */
void validate_options(const SearchOptions &options) {
        if (options.top_k > kMaxTopK) {
                throw std::invalid_argument("top-k exceeds maximum value");
        }
        if (!options.source_root.empty()) {
                std::error_code error;
                const bool directory = std::filesystem::is_directory(
                        options.source_root, error);
                if (error || !directory) {
                        throw std::invalid_argument(
                                "source root is not a readable directory");
                }
        }
}

/**
 * @brief Finds one posting by document identifier.
 * @param postings Ordered posting list.
 * @param document_id Identifier to locate.
 * @return Pointer to the posting, or nullptr when absent.
 */
[[nodiscard]] const index::Posting *
find_posting(const index::PostingList &postings,
             document::DocumentId document_id) {
        const auto iterator =
                std::lower_bound(postings.begin(), postings.end(), document_id,
                                 [](const index::Posting &posting,
                                    document::DocumentId candidate) {
                                         return posting.document_id < candidate;
                                 });
        return iterator == postings.end() ||
                               iterator->document_id != document_id
                       ? nullptr
                       : &*iterator;
}

/**
 * @brief Computes the mean indexed token count for BM25 normalization.
 * @param documents Corpus document table.
 * @return Mean token count, or zero for an empty corpus.
 */
[[nodiscard]] double
average_document_length(const document::DocumentStore &documents) {
        if (documents.size() == 0) {
                return 0.0;
        }
        long double sum = 0.0;
        for (const auto &document : documents.all()) {
                sum += document.token_count;
        }
        return static_cast<double>(sum / documents.size());
}

/**
 * @brief Narrows a corpus count for the current BM25 API.
 * @param value Count to narrow.
 * @param field Diagnostic field name.
 * @return Count represented as uint32_t.
 * @throws std::overflow_error If value exceeds uint32_t.
 */
[[nodiscard]] std::uint32_t checked_count(std::size_t value,
                                          std::string_view field) {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(std::string(field) +
                                          " exceeds uint32_t");
        }
        return static_cast<std::uint32_t>(value);
}

/**
 * @brief Computes BM25 and optional per-term details for one document.
 * @param documents Corpus document table.
 * @param index Positional index containing term frequencies.
 * @param document_id Document being scored.
 * @param terms Unique positive query terms.
 * @param corpus_average Mean indexed document length.
 * @param explanation Optional output vector for score details.
 * @return Sum of BM25 contributions for present terms.
 */
[[nodiscard]] double score_document(const document::DocumentStore &documents,
                                    const index::InMemoryIndex &index,
                                    document::DocumentId document_id,
                                    const std::vector<std::string> &terms,
                                    double corpus_average,
                                    std::vector<ScoreDetail> *explanation) {
        const auto document_count =
                checked_count(documents.size(), "document count");
        const auto document_length = documents.get(document_id).token_count;
        double total = 0.0;
        for (const auto &term : terms) {
                const auto *postings = index.find(term);
                if (postings == nullptr) {
                        continue;
                }
                const auto *posting = find_posting(*postings, document_id);
                if (posting == nullptr) {
                        continue;
                }
                const auto frequency = posting->term_frequency();
                const auto document_frequency =
                        checked_count(postings->size(), "document frequency");
                const double contribution = ranking::bm25(
                        frequency, document_frequency, document_length,
                        document_count, corpus_average);
                total += contribution;
                if (explanation != nullptr) {
                        explanation->push_back(ScoreDetail{term, frequency,
                                                           document_frequency,
                                                           contribution});
                }
        }
        return total;
}

/**
 * @brief Orders ranking candidates by score then path bytes.
 * @param left First candidate.
 * @param right Second candidate.
 * @return True when left should appear before right.
 */
[[nodiscard]] bool better_rank(const RankedDocument &left,
                               const RankedDocument &right) {
        return left.score != right.score ? left.score > right.score
                                         : left.path_key < right.path_key;
}

struct BetterRankComparator {
        /**
         * @brief Keeps the worst retained candidate at the heap top.
         * @param left First candidate.
         * @param right Second candidate.
         * @return True when left ranks ahead of right.
         */
        [[nodiscard]] bool operator()(const RankedDocument &left,
                                      const RankedDocument &right) const {
                return better_rank(left, right);
        }
};

/**
 * @brief Scores matches while retaining only the best requested candidates.
 * @param matches Boolean query matches.
 * @param documents Corpus document table.
 * @param index In-memory index used for BM25 inputs.
 * @param terms Unique positive query terms.
 * @param corpus_average Mean indexed document length.
 * @param top_k Maximum retained result count.
 * @return Ranked candidates ordered best first.
 */
[[nodiscard]] std::vector<RankedDocument>
rank_documents(const query::DocumentIds &matches,
               const document::DocumentStore &documents,
               const index::InMemoryIndex &index,
               const std::vector<std::string> &terms, double corpus_average,
               std::size_t top_k) {
        std::priority_queue<RankedDocument, std::vector<RankedDocument>,
                            BetterRankComparator>
                top_documents;
        for (const auto document_id : matches) {
                const auto &document = documents.get(document_id);
                RankedDocument candidate{
                        document_id,
                        score_document(documents, index, document_id, terms,
                                       corpus_average, nullptr),
                        document.path.generic_string()};
                if (top_documents.size() < top_k) {
                        top_documents.push(std::move(candidate));
                } else if (better_rank(candidate, top_documents.top())) {
                        top_documents.pop();
                        top_documents.push(std::move(candidate));
                }
        }

        std::vector<RankedDocument> ranked;
        ranked.reserve(top_documents.size());
        while (!top_documents.empty()) {
                ranked.push_back(top_documents.top());
                top_documents.pop();
        }
        std::sort(ranked.begin(), ranked.end(), better_rank);
        return ranked;
}

/**
 * @brief Verifies that a stored relative path cannot escape a source root.
 * @param path Stored document path.
 * @return True for nonempty relative paths without parent traversal.
 */
[[nodiscard]] bool is_safe_relative_path(const std::filesystem::path &path) {
        if (path.empty() || path.is_absolute()) {
                return false;
        }
        return std::none_of(
                path.begin(), path.end(),
                [](const auto &component) { return component == ".."; });
}

/**
 * @brief Tests whether a decoded line contains any positive query term.
 * @param line UTF-8 line contents.
 * @param terms Sorted normalized query terms.
 * @param tokenizer Tokenizer shared with indexing rules.
 * @return True for filter-only queries or a matching line token.
 */
[[nodiscard]] bool line_matches(std::string_view line,
                                const std::vector<std::string> &terms,
                                const analysis::Tokenizer &tokenizer) {
        if (terms.empty()) {
                return true;
        }
        for (const auto &token : tokenizer.tokenize(line)) {
                if (std::binary_search(terms.begin(), terms.end(), token)) {
                        return true;
                }
        }
        return false;
}

/**
 * @brief Truncates a valid UTF-8 line without splitting a code point.
 * @param line Complete decoded line.
 * @return At most 240 bytes, using an ASCII ellipsis when truncated.
 */
[[nodiscard]] std::string truncate_snippet(std::string line) {
        constexpr std::size_t kMaximumSnippetBytes = 240;
        constexpr std::size_t kEllipsisBytes = 3;
        if (line.size() <= kMaximumSnippetBytes) {
                return line;
        }
        std::size_t boundary = kMaximumSnippetBytes - kEllipsisBytes;
        while (boundary > 0 &&
               (static_cast<unsigned char>(line[boundary]) & 0xc0U) == 0x80U) {
                --boundary;
        }
        line.resize(boundary);
        line.append("...");
        return line;
}

/**
 * @brief Reads the first matching source line for one ranked document.
 * @param source_root Caller-supplied corpus root.
 * @param relative_path Validated relative path stored in the index.
 * @param terms Sorted positive terms used to choose a line.
 * @return One-based source snippet, or nullopt when unavailable.
 */
[[nodiscard]] std::optional<SourceSnippet>
load_snippet(const std::filesystem::path &source_root,
             const std::filesystem::path &relative_path,
             const std::vector<std::string> &terms) {
        if (!is_safe_relative_path(relative_path)) {
                return std::nullopt;
        }

        std::optional<SourceSnippet> result;
        std::string pending_line;
        std::size_t line_number = 1;
        const analysis::Tokenizer tokenizer;
        try {
                const document::TextReader reader;
                static_cast<void>(reader.read(
                        source_root / relative_path,
                        [&](std::string_view chunk) {
                                for (const char character : chunk) {
                                        if (character != '\n') {
                                                pending_line.push_back(
                                                        character);
                                                continue;
                                        }
                                        if (!pending_line.empty() &&
                                            pending_line.back() == '\r') {
                                                pending_line.pop_back();
                                        }
                                        if (line_matches(pending_line, terms,
                                                         tokenizer)) {
                                                result = SourceSnippet{
                                                        line_number,
                                                        truncate_snippet(
                                                                pending_line)};
                                                throw SnippetReady{};
                                        }
                                        pending_line.clear();
                                        ++line_number;
                                }
                        }));
                if (!pending_line.empty() &&
                    line_matches(pending_line, terms, tokenizer)) {
                        result = SourceSnippet{line_number,
                                               truncate_snippet(pending_line)};
                }
        } catch (const SnippetReady &) {
                return result;
        } catch (const std::exception &) {
                return std::nullopt;
        }
        return result;
}

} // namespace

class Searcher::Impl {
      public:
        /**
         * @brief Loads one stable visible index generation.
         * @param index_directory Directory containing the index.
         */
        explicit Impl(const std::filesystem::path &index_directory)
            : loaded(storage::read_index_directory(index_directory)) {}

        storage::LoadedIndex
                loaded; ///< Stable index generation used by searches.
};

Searcher::Searcher(const std::filesystem::path &index_directory)
    : impl_(std::make_unique<Impl>(index_directory)) {}

Searcher::~Searcher() = default;

Searcher::Searcher(Searcher &&) noexcept = default;

Searcher &Searcher::operator=(Searcher &&) noexcept = default;

std::vector<SearchHit> Searcher::search(std::string_view expression,
                                        const SearchOptions &options) const {
        if (impl_ == nullptr) {
                throw std::logic_error("cannot use a moved-from searcher");
        }
        validate_options(options);

        // Keep syntax and Boolean semantics independent of result limits.
        const auto parsed = query::parse_query(expression);
        if (options.top_k == 0) {
                return {};
        }
        const auto evaluation = query::evaluate_query(
                *parsed, impl_->loaded.documents, impl_->loaded.index);
        const double corpus_average =
                average_document_length(impl_->loaded.documents);

        // Rank only visible matches and retain no more than Top-K.
        const auto ranked =
                rank_documents(evaluation.documents, impl_->loaded.documents,
                               impl_->loaded.index, evaluation.positive_terms,
                               corpus_average, options.top_k);

        // Explanation and source I/O are presentation work for final hits only.
        std::vector<SearchHit> hits;
        hits.reserve(ranked.size());
        for (const auto &ranked_document : ranked) {
                const auto &document = impl_->loaded.documents.get(
                        ranked_document.document_id);
                SearchHit hit;
                hit.path = document.path;
                hit.score = ranked_document.score;
                if (options.explain) {
                        static_cast<void>(score_document(
                                impl_->loaded.documents, impl_->loaded.index,
                                ranked_document.document_id,
                                evaluation.positive_terms, corpus_average,
                                &hit.explanation));
                }
                if (!options.source_root.empty()) {
                        hit.snippet =
                                load_snippet(options.source_root, document.path,
                                             evaluation.positive_terms);
                }
                hits.push_back(std::move(hit));
        }
        return hits;
}

} // namespace snowseek
