#include "snowseek/query/query_engine.hpp"

#include "snowseek/analysis/tokenizer.hpp"
#include "snowseek/document/text_reader.hpp"
#include "snowseek/query/query_parser.hpp"
#include "snowseek/ranking/bm25.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fnmatch.h>
#include <iterator>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowseek::query {
namespace {

using DocumentIds = std::vector<document::DocumentId>;

struct RankedDocument {
        document::DocumentId document_id{};
        double score{};
        std::string path_key;
};

struct Snippet {
        std::size_t line{};
        std::string text;
};

struct SnippetReady final {};

/**
 * @brief Returns an ASCII-lowercase copy for extension comparison.
 * @param text Text whose ASCII letters are normalized.
 * @return Lowercase copy with non-ASCII bytes preserved.
 */
[[nodiscard]] std::string ascii_lower(std::string_view text) {
        std::string result(text);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char character) {
                               return static_cast<char>(
                                       std::tolower(character));
                       });
        return result;
}

/**
 * @brief Copies ordered document identifiers from a posting list.
 * @param postings Source posting list.
 * @return Strictly increasing document identifiers.
 */
[[nodiscard]] DocumentIds
posting_document_ids(const index::PostingList &postings) {
        DocumentIds result;
        result.reserve(postings.size());
        for (const auto &posting : postings) {
                result.push_back(posting.document_id);
        }
        return result;
}

/**
 * @brief Intersects two ordered identifier vectors.
 * @param left First ordered set.
 * @param right Second ordered set.
 * @return Ordered identifiers present in both inputs.
 */
[[nodiscard]] DocumentIds intersect_ids(const DocumentIds &left,
                                        const DocumentIds &right) {
        DocumentIds result;
        result.reserve(std::min(left.size(), right.size()));
        std::set_intersection(left.begin(), left.end(), right.begin(),
                              right.end(), std::back_inserter(result));
        return result;
}

/**
 * @brief Unites two ordered identifier vectors without duplicates.
 * @param left First ordered set.
 * @param right Second ordered set.
 * @return Ordered union of both inputs.
 */
[[nodiscard]] DocumentIds union_ids(const DocumentIds &left,
                                    const DocumentIds &right) {
        DocumentIds result;
        result.reserve(left.size() + right.size());
        std::set_union(left.begin(), left.end(), right.begin(), right.end(),
                       std::back_inserter(result));
        return result;
}

/**
 * @brief Subtracts an ordered identifier set from the corpus universe.
 * @param universe All valid document identifiers.
 * @param excluded Ordered identifiers to remove.
 * @return Ordered complement of excluded.
 */
[[nodiscard]] DocumentIds complement_ids(const DocumentIds &universe,
                                         const DocumentIds &excluded) {
        DocumentIds result;
        result.reserve(universe.size() -
                       std::min(universe.size(), excluded.size()));
        std::set_difference(universe.begin(), universe.end(), excluded.begin(),
                            excluded.end(), std::back_inserter(result));
        return result;
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
 * @brief Tests exact adjacent positions for all terms in one phrase.
 * @param postings Phrase posting lists in source term order.
 * @param document_id Candidate document identifier.
 * @return True when at least one exact phrase occurrence exists.
 */
[[nodiscard]] bool
phrase_matches(const std::vector<const index::PostingList *> &postings,
               document::DocumentId document_id) {
        std::vector<const index::Posting *> document_postings;
        document_postings.reserve(postings.size());
        for (const auto *posting_list : postings) {
                const auto *posting = find_posting(*posting_list, document_id);
                if (posting == nullptr) {
                        return false;
                }
                document_postings.push_back(posting);
        }

        for (const auto start : document_postings.front()->positions) {
                bool complete = true;
                for (std::size_t term_index = 1;
                     term_index < document_postings.size(); ++term_index) {
                        if (start >
                            std::numeric_limits<index::Position>::max() -
                                    term_index) {
                                complete = false;
                                break;
                        }
                        const auto expected = static_cast<index::Position>(
                                start + term_index);
                        if (!std::binary_search(document_postings[term_index]
                                                        ->positions.begin(),
                                                document_postings[term_index]
                                                        ->positions.end(),
                                                expected)) {
                                complete = false;
                                break;
                        }
                }
                if (complete) {
                        return true;
                }
        }
        return false;
}

/**
 * @brief Flattens nested conjunctions for selectivity ordering.
 * @param node Expression subtree to inspect.
 * @param operands Output sequence receiving non-AND operands.
 */
void collect_conjuncts(const QueryNode &node,
                       std::vector<const QueryNode *> &operands) {
        if (node.kind == QueryNodeKind::conjunction) {
                collect_conjuncts(*node.left, operands);
                collect_conjuncts(*node.right, operands);
                return;
        }
        operands.push_back(&node);
}

class Evaluator {
      public:
        /**
         * @brief Binds Boolean evaluation to immutable corpus structures.
         * @param documents Contiguous document metadata table.
         * @param index In-memory positional inverted index.
         */
        Evaluator(const document::DocumentStore &documents,
                  const index::InMemoryIndex &index)
            : documents_(documents), index_(index) {
                universe_.reserve(documents.size());
                for (std::size_t id = 0; id < documents.size(); ++id) {
                        universe_.push_back(
                                static_cast<document::DocumentId>(id));
                }
        }

        /**
         * @brief Evaluates an AST into a sorted document set.
         * @param node Root expression node.
         * @return Matching identifiers in ascending order.
         */
        [[nodiscard]] DocumentIds evaluate(const QueryNode &node) const {
                switch (node.kind) {
                case QueryNodeKind::term:
                        return evaluate_term(node.value);
                case QueryNodeKind::phrase:
                        return evaluate_phrase(node.terms);
                case QueryNodeKind::path_filter:
                        return evaluate_path_filter(node.value);
                case QueryNodeKind::extension_filter:
                        return evaluate_extension_filter(node.value);
                case QueryNodeKind::conjunction:
                        return evaluate_conjunction(node);
                case QueryNodeKind::disjunction:
                        return union_ids(evaluate(*node.left),
                                         evaluate(*node.right));
                case QueryNodeKind::negation:
                        return complement_ids(universe_, evaluate(*node.left));
                }
                throw std::logic_error("unknown query node kind");
        }

      private:
        /** @brief Evaluates one normalized term lookup. */
        [[nodiscard]] DocumentIds evaluate_term(std::string_view term) const {
                const auto *postings = index_.find(term);
                return postings == nullptr ? DocumentIds{}
                                           : posting_document_ids(*postings);
        }

        /** @brief Evaluates an exact phrase using posting positions. */
        [[nodiscard]] DocumentIds
        evaluate_phrase(const std::vector<std::string> &terms) const {
                if (!index_.stores_positions()) {
                        throw std::invalid_argument(
                                "index does not contain positions required for "
                                "phrase queries");
                }
                std::vector<const index::PostingList *> posting_lists;
                posting_lists.reserve(terms.size());
                for (const auto &term : terms) {
                        const auto *postings = index_.find(term);
                        if (postings == nullptr) {
                                return {};
                        }
                        posting_lists.push_back(postings);
                }

                const auto smallest = std::min_element(
                        posting_lists.begin(), posting_lists.end(),
                        [](const auto *left, const auto *right) {
                                return left->size() < right->size();
                        });
                DocumentIds candidates = posting_document_ids(**smallest);
                for (const auto *postings : posting_lists) {
                        candidates = intersect_ids(
                                candidates, posting_document_ids(*postings));
                        if (candidates.empty()) {
                                return {};
                        }
                }

                DocumentIds matches;
                for (const auto document_id : candidates) {
                        if (phrase_matches(posting_lists, document_id)) {
                                matches.push_back(document_id);
                        }
                }
                return matches;
        }

        /** @brief Evaluates a case-sensitive Glob against relative paths. */
        [[nodiscard]] DocumentIds
        evaluate_path_filter(const std::string &pattern) const {
                DocumentIds matches;
                const std::string owned_pattern(pattern);
                for (const auto &document : documents_.all()) {
                        const std::string path = document.path.generic_string();
                        if (::fnmatch(owned_pattern.c_str(), path.c_str(), 0) ==
                            0) {
                                matches.push_back(document.id);
                        }
                }
                return matches;
        }

        /** @brief Evaluates a case-insensitive exact extension filter. */
        [[nodiscard]] DocumentIds
        evaluate_extension_filter(std::string extension) const {
                if (!extension.empty() && extension.front() == '.') {
                        extension.erase(extension.begin());
                }
                extension = ascii_lower(extension);
                DocumentIds matches;
                for (const auto &document : documents_.all()) {
                        std::string candidate =
                                document.path.extension().generic_string();
                        if (!candidate.empty() && candidate.front() == '.') {
                                candidate.erase(candidate.begin());
                        }
                        if (ascii_lower(candidate) == extension) {
                                matches.push_back(document.id);
                        }
                }
                return matches;
        }

        /** @brief Evaluates flattened AND operands from most selective first.
         */
        [[nodiscard]] DocumentIds
        evaluate_conjunction(const QueryNode &node) const {
                std::vector<const QueryNode *> operands;
                collect_conjuncts(node, operands);
                std::stable_sort(
                        operands.begin(), operands.end(),
                        [this](const QueryNode *left, const QueryNode *right) {
                                return estimate(*left) < estimate(*right);
                        });
                DocumentIds matches = evaluate(*operands.front());
                for (std::size_t index = 1;
                     index < operands.size() && !matches.empty(); ++index) {
                        matches = intersect_ids(matches,
                                                evaluate(*operands[index]));
                }
                return matches;
        }

        /** @brief Estimates result cardinality for AND ordering. */
        [[nodiscard]] std::size_t estimate(const QueryNode &node) const {
                switch (node.kind) {
                case QueryNodeKind::term: {
                        const auto *postings = index_.find(node.value);
                        return postings == nullptr ? 0 : postings->size();
                }
                case QueryNodeKind::phrase: {
                        std::size_t result = documents_.size();
                        for (const auto &term : node.terms) {
                                const auto *postings = index_.find(term);
                                if (postings == nullptr) {
                                        return 0;
                                }
                                result = std::min(result, postings->size());
                        }
                        return result;
                }
                case QueryNodeKind::conjunction:
                        return std::min(estimate(*node.left),
                                        estimate(*node.right));
                case QueryNodeKind::disjunction: {
                        const auto left = estimate(*node.left);
                        const auto right = estimate(*node.right);
                        if (left >= documents_.size() ||
                            right > documents_.size() - left) {
                                return documents_.size();
                        }
                        return left + right;
                }
                case QueryNodeKind::path_filter:
                case QueryNodeKind::extension_filter:
                case QueryNodeKind::negation:
                        return documents_.size();
                }
                return documents_.size();
        }

        const document::DocumentStore &documents_;
        const index::InMemoryIndex &index_;
        DocumentIds universe_;
};

/**
 * @brief Collects unique scoreable terms outside effective negation.
 * @param node Query subtree to inspect.
 * @param negated Whether the subtree is under an odd number of NOT nodes.
 * @param terms Output vector receiving normalized terms.
 */
void collect_positive_terms(const QueryNode &node, bool negated,
                            std::vector<std::string> &terms) {
        switch (node.kind) {
        case QueryNodeKind::term:
                if (!negated) {
                        terms.push_back(node.value);
                }
                return;
        case QueryNodeKind::phrase:
                if (!negated) {
                        terms.insert(terms.end(), node.terms.begin(),
                                     node.terms.end());
                }
                return;
        case QueryNodeKind::negation:
                collect_positive_terms(*node.left, !negated, terms);
                return;
        case QueryNodeKind::conjunction:
        case QueryNodeKind::disjunction:
                collect_positive_terms(*node.left, negated, terms);
                collect_positive_terms(*node.right, negated, terms);
                return;
        case QueryNodeKind::path_filter:
        case QueryNodeKind::extension_filter:
                return;
        }
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
 * @brief Computes BM25 and optional per-term explanation for one document.
 * @param documents Corpus document table.
 * @param index Positional index containing term frequencies.
 * @param document_id Document being scored.
 * @param terms Unique positive query terms.
 * @param corpus_average Mean indexed document length.
 * @param explanation Optional output vector for score contributions.
 * @return Sum of BM25 contributions for present terms.
 */
[[nodiscard]] double
score_document(const document::DocumentStore &documents,
               const index::InMemoryIndex &index,
               document::DocumentId document_id,
               const std::vector<std::string> &terms, double corpus_average,
               std::vector<ScoreContribution> *explanation) {
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
                        explanation->push_back(ScoreContribution{
                                term, frequency, document_frequency,
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
        /** @brief Keeps the worst retained candidate at the heap top. */
        [[nodiscard]] bool operator()(const RankedDocument &left,
                                      const RankedDocument &right) const {
                return better_rank(left, right);
        }
};

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
 * @return One-based line and snippet, or an empty value when unavailable.
 */
[[nodiscard]] Snippet load_snippet(const std::filesystem::path &source_root,
                                   const std::filesystem::path &relative_path,
                                   const std::vector<std::string> &terms) {
        if (!is_safe_relative_path(relative_path)) {
                return {};
        }

        Snippet result;
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
                                                result = Snippet{
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
                        result = Snippet{line_number,
                                         truncate_snippet(pending_line)};
                }
        } catch (const SnippetReady &) {
                return result;
        } catch (const std::exception &) {
                return {};
        }
        return result;
}

} // namespace

QueryEngine::QueryEngine(const std::filesystem::path &index_directory)
    : loaded_(storage::read_index_directory(index_directory)) {}

std::vector<SearchResult>
QueryEngine::search(std::string_view expression,
                    const SearchOptions &options) const {
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

        // Parse and evaluate before ranking so syntax and Boolean semantics are
        // independent of result limits and presentation options.
        const auto query = parse_query(expression);
        if (options.top_k == 0) {
                return {};
        }
        const Evaluator evaluator(loaded_.documents, loaded_.index);
        const auto matches = evaluator.evaluate(*query);

        std::vector<std::string> positive_terms;
        collect_positive_terms(*query, false, positive_terms);
        std::sort(positive_terms.begin(), positive_terms.end());
        positive_terms.erase(
                std::unique(positive_terms.begin(), positive_terms.end()),
                positive_terms.end());
        const double corpus_average =
                average_document_length(loaded_.documents);

        // Retain only the best K candidates while scoring the Boolean result.
        std::priority_queue<RankedDocument, std::vector<RankedDocument>,
                            BetterRankComparator>
                top_documents;
        for (const auto document_id : matches) {
                const auto &document = loaded_.documents.get(document_id);
                RankedDocument candidate{
                        document_id,
                        score_document(loaded_.documents, loaded_.index,
                                       document_id, positive_terms,
                                       corpus_average, nullptr),
                        document.path.generic_string()};
                if (top_documents.size() < options.top_k) {
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

        // Only final Top-K documents may touch original source files.
        std::vector<SearchResult> results;
        results.reserve(ranked.size());
        for (const auto &ranked_document : ranked) {
                const auto &document =
                        loaded_.documents.get(ranked_document.document_id);
                SearchResult result;
                result.path = document.path;
                result.score = ranked_document.score;
                if (options.explain) {
                        static_cast<void>(score_document(
                                loaded_.documents, loaded_.index,
                                ranked_document.document_id, positive_terms,
                                corpus_average, &result.explanation));
                }
                if (!options.source_root.empty()) {
                        auto snippet =
                                load_snippet(options.source_root, document.path,
                                             positive_terms);
                        result.line = snippet.line;
                        result.snippet = std::move(snippet.text);
                }
                results.push_back(std::move(result));
        }
        return results;
}

} // namespace snowseek::query
