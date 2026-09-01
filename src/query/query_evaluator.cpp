/**
 * @file query_evaluator.cpp
 * @brief Evaluates Boolean query trees against an immutable in-memory corpus.
 */

#include "query/query_evaluator.hpp"

#include "document/document_store.hpp"
#include "index/index.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fnmatch.h>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowseek::query {
namespace {

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
 * @brief Tests ordered positions within one phrase's permitted extra span.
 * @param postings Phrase posting lists in source term order.
 * @param document_id Candidate document identifier.
 * @param proximity Maximum positions beyond an exact adjacent phrase.
 * @return True when at least one ordered phrase occurrence exists.
 */
[[nodiscard]] bool
phrase_matches(const std::vector<const index::PostingList *> &postings,
               document::DocumentId document_id, std::uint32_t proximity) {
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
                auto previous = start;
                bool complete = true;
                for (std::size_t term_index = 1;
                     term_index < document_postings.size(); ++term_index) {
                        const auto &positions =
                                document_postings[term_index]->positions;
                        const auto next = std::upper_bound(
                                positions.begin(), positions.end(), previous);
                        if (next == positions.end()) {
                                complete = false;
                                break;
                        }
                        previous = *next;
                }
                const auto exact_span = document_postings.size() - 1;
                if (complete &&
                    static_cast<std::uint64_t>(previous) - start - exact_span <=
                            proximity) {
                        return true;
                }
        }
        return false;
}

/**
 * @brief Applies one query comparison to scalar metadata values.
 * @tparam Value Integer metadata type.
 * @param candidate Document value.
 * @param expected Query value.
 * @param comparison Requested relation.
 * @return True when the relation holds.
 */
template <typename Value>
[[nodiscard]] bool compare(Value candidate, Value expected,
                           ComparisonOperator comparison) {
        switch (comparison) {
        case ComparisonOperator::equal:
                return candidate == expected;
        case ComparisonOperator::not_equal:
                return candidate != expected;
        case ComparisonOperator::less:
                return candidate < expected;
        case ComparisonOperator::less_equal:
                return candidate <= expected;
        case ComparisonOperator::greater:
                return candidate > expected;
        case ComparisonOperator::greater_equal:
                return candidate >= expected;
        }
        return false;
}

/**
 * @brief Converts Epoch nanoseconds to a UTC day using floor division.
 * @param nanoseconds Signed Unix Epoch nanoseconds.
 * @return Day ordinal where 1970-01-01 is zero.
 */
[[nodiscard]] std::int64_t utc_day(std::int64_t nanoseconds) noexcept {
        constexpr std::int64_t nanoseconds_per_day = 86400000000000LL;
        auto day = nanoseconds / nanoseconds_per_day;
        if (nanoseconds % nanoseconds_per_day < 0) {
                --day;
        }
        return day;
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
            : documents_(documents), index_(index),
              dictionary_terms_(index.sorted_terms()) {
                universe_.reserve(documents.size());
                for (std::size_t id = 0; id < documents.size(); ++id) {
                        universe_.push_back(
                                static_cast<document::DocumentId>(id));
                }
        }

        /**
         * @brief Expands prefixes once, evaluates matches, and gathers scores.
         * @param query Root query expression.
         * @return Matching documents and sorted unique positive terms.
         * @throws std::invalid_argument If prefix expansion exceeds its limit.
         */
        [[nodiscard]] QueryEvaluation run(const QueryNode &query) {
                std::set<std::string> expanded_terms;
                prepare_prefixes(query, expanded_terms);
                std::vector<std::string> positive_terms;
                collect_positive_terms(query, false, positive_terms);
                std::sort(positive_terms.begin(), positive_terms.end());
                positive_terms.erase(std::unique(positive_terms.begin(),
                                                 positive_terms.end()),
                                     positive_terms.end());
                return QueryEvaluation{evaluate(query),
                                       std::move(positive_terms)};
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
                case QueryNodeKind::prefix:
                        return evaluate_prefix(node);
                case QueryNodeKind::phrase:
                        return evaluate_phrase(node.terms, node.proximity);
                case QueryNodeKind::path_filter:
                        return evaluate_path_filter(node.value);
                case QueryNodeKind::extension_filter:
                        return evaluate_extension_filter(node.value);
                case QueryNodeKind::size_filter:
                        return evaluate_size_filter(node);
                case QueryNodeKind::mtime_filter:
                        return evaluate_mtime_filter(node);
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
        /**
         * @brief Evaluates one normalized term lookup.
         * @param term Normalized dictionary term.
         * @return Ordered matching document identifiers.
         */
        [[nodiscard]] DocumentIds evaluate_term(std::string_view term) const {
                const auto *postings = index_.find(term);
                return postings == nullptr ? DocumentIds{}
                                           : posting_document_ids(*postings);
        }

        /**
         * @brief Unites posting lists for one previously expanded prefix.
         * @param node Prefix AST node prepared by run().
         * @return Ordered identifiers containing any expanded term.
         */
        [[nodiscard]] DocumentIds evaluate_prefix(const QueryNode &node) const {
                DocumentIds matches;
                const auto expansion = prefix_expansions_.find(&node);
                if (expansion == prefix_expansions_.end()) {
                        throw std::logic_error("prefix was not prepared");
                }
                for (const auto &term : expansion->second) {
                        matches = union_ids(matches, evaluate_term(term));
                }
                return matches;
        }

        /**
         * @brief Evaluates an ordered phrase using posting positions.
         * @param terms Phrase terms in source order.
         * @param proximity Maximum positions beyond exact adjacency.
         * @return Ordered documents containing a permitted occurrence.
         */
        [[nodiscard]] DocumentIds
        evaluate_phrase(const std::vector<std::string> &terms,
                        std::uint32_t proximity) const {
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
                        if (phrase_matches(posting_lists, document_id,
                                           proximity)) {
                                matches.push_back(document_id);
                        }
                }
                return matches;
        }

        /**
         * @brief Evaluates a case-sensitive Glob against relative paths.
         * @param pattern Glob pattern retained by the query.
         * @return Ordered documents whose relative paths match.
         */
        [[nodiscard]] DocumentIds
        evaluate_path_filter(const std::string &pattern) const {
                DocumentIds matches;
                for (const auto &document : documents_.all()) {
                        const std::string path = document.path.generic_string();
                        if (::fnmatch(pattern.c_str(), path.c_str(), 0) == 0) {
                                matches.push_back(document.id);
                        }
                }
                return matches;
        }

        /**
         * @brief Evaluates a case-insensitive exact extension filter.
         * @param extension Extension with an optional leading dot.
         * @return Ordered documents having the requested extension.
         */
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

        /**
         * @brief Evaluates a comparison against indexed source byte lengths.
         * @param node Parsed size-filter node.
         * @return Ordered documents satisfying the comparison.
         */
        [[nodiscard]] DocumentIds
        evaluate_size_filter(const QueryNode &node) const {
                DocumentIds matches;
                for (const auto &document : documents_.all()) {
                        if (compare(document.file_size, node.size_bytes,
                                    node.comparison)) {
                                matches.push_back(document.id);
                        }
                }
                return matches;
        }

        /**
         * @brief Evaluates a comparison against indexed UTC modification days.
         * @param node Parsed mtime-filter node.
         * @return Ordered documents satisfying the comparison.
         */
        [[nodiscard]] DocumentIds
        evaluate_mtime_filter(const QueryNode &node) const {
                DocumentIds matches;
                for (const auto &document : documents_.all()) {
                        if (compare(utc_day(document.modified_time_ns),
                                    node.mtime_day, node.comparison)) {
                                matches.push_back(document.id);
                        }
                }
                return matches;
        }

        /**
         * @brief Expands every prefix node and enforces the query-wide bound.
         * @param node Query subtree to prepare.
         * @param distinct_terms Concrete prefix terms seen across the query.
         * @throws std::invalid_argument If more than 256 terms are expanded.
         */
        void prepare_prefixes(const QueryNode &node,
                              std::set<std::string> &distinct_terms) {
                if (node.kind == QueryNodeKind::prefix) {
                        const auto first = std::lower_bound(
                                dictionary_terms_.begin(),
                                dictionary_terms_.end(), node.value);
                        auto &expansion = prefix_expansions_[&node];
                        for (auto term = first;
                             term != dictionary_terms_.end() &&
                             term->starts_with(node.value);
                             ++term) {
                                expansion.push_back(*term);
                                distinct_terms.insert(*term);
                                if (distinct_terms.size() >
                                    kMaxExpandedPrefixTerms) {
                                        throw std::invalid_argument(
                                                "query prefix expansion "
                                                "exceeds 256 distinct terms");
                                }
                        }
                }
                if (node.left != nullptr) {
                        prepare_prefixes(*node.left, distinct_terms);
                }
                if (node.right != nullptr) {
                        prepare_prefixes(*node.right, distinct_terms);
                }
        }

        /**
         * @brief Collects scoreable concrete terms outside effective NOT.
         * @param node Query subtree to inspect.
         * @param negated Whether an odd number of NOT nodes applies.
         * @param terms Output receiving normalized concrete terms.
         */
        void collect_positive_terms(const QueryNode &node, bool negated,
                                    std::vector<std::string> &terms) const {
                switch (node.kind) {
                case QueryNodeKind::term:
                        if (!negated) {
                                terms.push_back(node.value);
                        }
                        return;
                case QueryNodeKind::prefix:
                        if (!negated) {
                                const auto &expansion =
                                        prefix_expansions_.at(&node);
                                terms.insert(terms.end(), expansion.begin(),
                                             expansion.end());
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
                case QueryNodeKind::size_filter:
                case QueryNodeKind::mtime_filter:
                        return;
                }
        }

        /**
         * @brief Evaluates flattened AND operands from most selective first.
         * @param node Conjunction subtree.
         * @return Ordered documents matching every operand.
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

        /**
         * @brief Estimates result cardinality for AND ordering.
         * @param node Query subtree to estimate.
         * @return Upper-bound-like cardinality estimate.
         */
        [[nodiscard]] std::size_t estimate(const QueryNode &node) const {
                switch (node.kind) {
                case QueryNodeKind::term: {
                        const auto *postings = index_.find(node.value);
                        return postings == nullptr ? 0 : postings->size();
                }
                case QueryNodeKind::prefix: {
                        std::size_t result = 0;
                        for (const auto &term : prefix_expansions_.at(&node)) {
                                const auto *postings = index_.find(term);
                                if (postings == nullptr ||
                                    result >=
                                            documents_.size() -
                                                    std::min(
                                                            documents_.size(),
                                                            postings->size())) {
                                        return documents_.size();
                                }
                                result += postings->size();
                        }
                        return result;
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
                case QueryNodeKind::size_filter:
                case QueryNodeKind::mtime_filter:
                case QueryNodeKind::negation:
                        return documents_.size();
                }
                return documents_.size();
        }

        const document::DocumentStore
                &documents_;                ///< Immutable visible corpus.
        const index::InMemoryIndex &index_; ///< Immutable visible postings.
        std::vector<std::string> dictionary_terms_; ///< Sorted vocabulary copy.
        std::unordered_map<const QueryNode *, std::vector<std::string>>
                prefix_expansions_; ///< Concrete terms for each prefix node.
        DocumentIds universe_;      ///< Every visible identifier in order.
};

} // namespace

QueryEvaluation evaluate_query(const QueryNode &query,
                               const document::DocumentStore &documents,
                               const index::InMemoryIndex &index) {
        return Evaluator(documents, index).run(query);
}

} // namespace snowseek::query
