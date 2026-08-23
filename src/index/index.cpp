#include "snowseek/index/index.hpp"

#include "snowseek/common/checked_arithmetic.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace snowseek::index {
namespace {

using common::detail::checked_add;
using common::detail::checked_multiply;

} // namespace

std::uint32_t Posting::term_frequency() const {
        if (positions.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(
                        "posting term frequency exceeds uint32_t");
        }
        return static_cast<std::uint32_t>(positions.size());
}

void InMemoryIndex::add_occurrence(std::string_view term,
                                   document::DocumentId document_id,
                                   Position position) {
        if (term.empty()) {
                throw std::invalid_argument("index term must not be empty");
        }

        auto [iterator, inserted] =
                dictionary_.try_emplace(std::string(term), PostingList{});
        auto &postings = iterator->second;

        if (inserted || postings.empty()) {
                postings.push_back(Posting{document_id, {position}});
                return;
        }

        Posting &last_posting = postings.back();
        if (document_id < last_posting.document_id) {
                throw std::invalid_argument(
                        "posting document ids must be strictly increasing");
        }

        if (document_id > last_posting.document_id) {
                postings.push_back(Posting{document_id, {position}});
                return;
        }

        if (position <= last_posting.positions.back()) {
                throw std::invalid_argument(
                        "posting positions must be strictly increasing");
        }
        if (last_posting.positions.size() >=
            std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(
                        "posting term frequency exceeds uint32_t");
        }
        last_posting.positions.push_back(position);
}

const PostingList *InMemoryIndex::find(std::string_view term) const {
        const auto iterator = dictionary_.find(std::string(term));
        return iterator == dictionary_.end() ? nullptr : &iterator->second;
}

std::size_t InMemoryIndex::term_count() const noexcept {
        return dictionary_.size();
}

std::vector<std::string> InMemoryIndex::sorted_terms() const {
        std::vector<std::string> terms;
        terms.reserve(dictionary_.size());
        for (const auto &[term, postings] : dictionary_) {
                static_cast<void>(postings);
                terms.push_back(term);
        }
        std::sort(terms.begin(), terms.end());
        return terms;
}

InMemoryIndexMemoryUsage InMemoryIndex::estimated_memory_usage() const {
        if (dictionary_.empty()) {
                return {};
        }

        InMemoryIndexMemoryUsage usage;
        usage.dictionary_bytes = checked_multiply(dictionary_.bucket_count(),
                                                  sizeof(void *), "dictionary");
        usage.dictionary_bytes = checked_add(
                usage.dictionary_bytes,
                checked_multiply(
                        dictionary_.size(),
                        sizeof(typename decltype(dictionary_)::value_type),
                        "dictionary"),
                "dictionary");

        for (const auto &[term, postings] : dictionary_) {
                usage.dictionary_bytes = checked_add(
                        usage.dictionary_bytes,
                        checked_multiply(term.capacity(), sizeof(char),
                                         "dictionary term"),
                        "dictionary");
                usage.posting_bytes = checked_add(
                        usage.posting_bytes,
                        checked_multiply(postings.capacity(), sizeof(Posting),
                                         "posting list"),
                        "posting");
                for (const auto &posting : postings) {
                        usage.posting_bytes = checked_add(
                                usage.posting_bytes,
                                checked_multiply(posting.positions.capacity(),
                                                 sizeof(Position), "position"),
                                "posting");
                }
        }
        return usage;
}

} // namespace snowseek::index
