#include "snowseek/query/query_engine.hpp"

#include "snowseek/query/posting_operations.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowseek::query {
namespace {

/**
 * @brief Normalizes a query input and enforces the one-token M1 grammar.
 * @param tokenizer Tokenizer defining normalization and length limits.
 * @param input Query input to normalize.
 * @return The single normalized term.
 * @throws std::invalid_argument If input produces zero or multiple tokens.
 * @throws std::length_error If input exceeds the tokenizer limit.
 */
[[nodiscard]] std::string
normalize_query_term(const analysis::Tokenizer &tokenizer,
                     std::string_view input) {
        auto tokens = tokenizer.tokenize(input);
        if (tokens.size() != 1) {
                throw std::invalid_argument(
                        "query input must produce exactly one token");
        }
        return std::move(tokens.front());
}

} // namespace

QueryEngine::QueryEngine(std::filesystem::path index_directory)
    : index_directory_(std::move(index_directory)),
      loaded_(storage::read_index_file(index_directory_ /
                                       storage::kSegmentFileName)) {}

std::vector<SearchResult> QueryEngine::search(std::string_view expression,
                                              std::size_t top_k) const {
        if (expression.empty()) {
                throw std::invalid_argument(
                        "query expression must not be empty");
        }
        if (top_k == 0) {
                return {};
        }

        // M2 accepts either one token-shaped input or two operands joined by a
        // case-insensitive AND keyword. Full parsing remains M3 work.
        std::istringstream parser{std::string(expression)};
        std::vector<std::string> fields;
        for (std::string field; parser >> field;) {
                fields.push_back(std::move(field));
        }
        const InMemoryQueryEngine engine(loaded_.documents, loaded_.index);
        std::vector<SearchResult> results;
        if (fields.size() == 1) {
                const auto matches = engine.search_term(fields[0]);
                results.reserve(std::min(top_k, matches.size()));
                for (const auto &match : matches) {
                        if (results.size() == top_k) {
                                break;
                        }
                        results.push_back(SearchResult{match.path, 0, 0.0, {}});
                }
                return results;
        }
        std::string operation = fields.size() == 3 ? fields[1] : std::string{};
        std::transform(operation.begin(), operation.end(), operation.begin(),
                       [](unsigned char character) {
                               return static_cast<char>(
                                       std::toupper(character));
                       });
        if (fields.size() != 3 || operation != "AND") {
                throw std::invalid_argument(
                        "query must be one term or 'term AND term'");
        }
        const auto matches = engine.search_and(fields[0], fields[2]);
        results.reserve(std::min(top_k, matches.size()));
        for (const auto &match : matches) {
                if (results.size() == top_k) {
                        break;
                }
                results.push_back(SearchResult{match.path, 0, 0.0, {}});
        }
        return results;
}

InMemoryQueryEngine::InMemoryQueryEngine(
        const document::DocumentStore &documents,
        const index::InMemoryIndex &index,
        analysis::TokenizerOptions tokenizer_options)
    : documents_(documents), index_(index), tokenizer_(tokenizer_options) {}

std::vector<InMemoryTermMatch>
InMemoryQueryEngine::search_term(std::string_view term) const {
        const auto normalized = normalize_query_term(tokenizer_, term);
        const auto *postings = index_.find(normalized);
        if (postings == nullptr) {
                return {};
        }

        std::vector<InMemoryTermMatch> matches;
        matches.reserve(postings->size());
        for (const auto &posting : *postings) {
                const auto &document = documents_.get(posting.document_id);
                matches.push_back(InMemoryTermMatch{
                        posting.document_id, document.path,
                        posting.term_frequency(), posting.positions});
        }
        return matches;
}

std::vector<InMemoryDocumentMatch>
InMemoryQueryEngine::search_and(std::string_view left,
                                std::string_view right) const {
        const auto normalized_left = normalize_query_term(tokenizer_, left);
        const auto normalized_right = normalize_query_term(tokenizer_, right);
        const auto *left_postings = index_.find(normalized_left);
        const auto *right_postings = index_.find(normalized_right);
        if (left_postings == nullptr || right_postings == nullptr) {
                return {};
        }

        const auto document_ids =
                intersect_document_ids(*left_postings, *right_postings);
        std::vector<InMemoryDocumentMatch> matches;
        matches.reserve(document_ids.size());
        for (const auto document_id : document_ids) {
                matches.push_back(InMemoryDocumentMatch{
                        document_id, documents_.get(document_id).path});
        }
        return matches;
}

} // namespace snowseek::query
