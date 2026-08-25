#include "snowseek/index/index.hpp"

#include "snowseek/common/checked_arithmetic.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

namespace snowseek::index {
namespace {

using common::detail::checked_add;
using common::detail::checked_multiply;

} // namespace

InMemoryIndex::InMemoryIndex(bool store_positions) noexcept
    : store_positions_(store_positions) {}

std::uint32_t Posting::term_frequency() const { return frequency; }

void InMemoryIndex::add_occurrence(std::string_view term,
                                   document::DocumentId document_id,
                                   Position position) {
        auto &postings = postings_for(term);
        if (postings.empty() || document_id > postings.back().document_id) {
                append_posting(postings,
                               Posting{document_id, 1,
                                       store_positions_
                                               ? std::vector<Position>{position}
                                               : std::vector<Position>{}});
                return;
        }

        Posting &last_posting = postings.back();
        if (document_id < last_posting.document_id) {
                throw std::invalid_argument(
                        "posting document ids must be strictly increasing");
        }

        if (store_positions_ && position <= last_posting.positions.back()) {
                throw std::invalid_argument(
                        "posting positions must be strictly increasing");
        }
        if (last_posting.frequency ==
            std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(
                        "posting term frequency exceeds uint32_t");
        }
        if (store_positions_) {
                const auto old_capacity = last_posting.positions.capacity();
                last_posting.positions.push_back(position);
                memory_usage_.posting_bytes = checked_add(
                        memory_usage_.posting_bytes,
                        checked_multiply(last_posting.positions.capacity() -
                                                 old_capacity,
                                         sizeof(Position), "position"),
                        "posting");
        }
        ++last_posting.frequency;
}

void InMemoryIndex::add_posting(std::string_view term,
                                document::DocumentId document_id,
                                std::uint32_t frequency,
                                std::vector<Position> positions) {
        if (frequency == 0) {
                throw std::invalid_argument("index posting requires frequency");
        }
        if ((store_positions_ && positions.size() != frequency) ||
            (!store_positions_ && !positions.empty()) ||
            std::adjacent_find(positions.begin(), positions.end(),
                               std::greater_equal<Position>{}) !=
                    positions.end()) {
                throw std::invalid_argument("index posting positions invalid");
        }

        auto &postings = postings_for(term);
        if (!postings.empty() && document_id <= postings.back().document_id) {
                throw std::invalid_argument(
                        "posting document ids must be strictly increasing");
        }
        append_posting(postings,
                       Posting{document_id, frequency, std::move(positions)});
}

PostingList &InMemoryIndex::postings_for(std::string_view term) {
        if (term.empty()) {
                throw std::invalid_argument("index term must not be empty");
        }

        const bool was_empty = dictionary_.empty();
        const auto old_bucket_count = dictionary_.bucket_count();
        auto [iterator, inserted] =
                dictionary_.try_emplace(std::string(term), PostingList{});
        const auto counted_bucket_count = was_empty ? 0 : old_bucket_count;
        if (dictionary_.bucket_count() != counted_bucket_count) {
                memory_usage_.dictionary_bytes = checked_add(
                        memory_usage_.dictionary_bytes,
                        checked_multiply(dictionary_.bucket_count() -
                                                 counted_bucket_count,
                                         sizeof(void *), "dictionary"),
                        "dictionary");
        }
        if (inserted) {
                memory_usage_.dictionary_bytes = checked_add(
                        memory_usage_.dictionary_bytes,
                        sizeof(typename decltype(dictionary_)::value_type),
                        "dictionary");
                memory_usage_.dictionary_bytes = checked_add(
                        memory_usage_.dictionary_bytes,
                        checked_multiply(iterator->first.capacity(),
                                         sizeof(char), "dictionary term"),
                        "dictionary");
        }
        return iterator->second;
}

void InMemoryIndex::append_posting(PostingList &postings, Posting posting) {
        const auto old_capacity = postings.capacity();
        postings.push_back(std::move(posting));
        memory_usage_.posting_bytes =
                checked_add(memory_usage_.posting_bytes,
                            checked_multiply(postings.capacity() - old_capacity,
                                             sizeof(Posting), "posting list"),
                            "posting");
        memory_usage_.posting_bytes = checked_add(
                memory_usage_.posting_bytes,
                checked_multiply(postings.back().positions.capacity(),
                                 sizeof(Position), "position"),
                "posting");
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
        return memory_usage_;
}

bool InMemoryIndex::stores_positions() const noexcept {
        return store_positions_;
}

} // namespace snowseek::index
