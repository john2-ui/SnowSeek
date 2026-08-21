#include "snowseek/query/posting_operations.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace snowseek::query {

std::vector<document::DocumentId>
intersect_document_ids(const index::PostingList &left,
                       const index::PostingList &right) {
        std::vector<document::DocumentId> matches;
        matches.reserve(std::min(left.size(), right.size()));

        std::size_t left_index = 0;
        std::size_t right_index = 0;
        while (left_index < left.size() && right_index < right.size()) {
                const auto left_id = left[left_index].document_id;
                const auto right_id = right[right_index].document_id;
                if (left_id < right_id) {
                        ++left_index;
                } else if (right_id < left_id) {
                        ++right_index;
                } else {
                        matches.push_back(left_id);
                        ++left_index;
                        ++right_index;
                }
        }
        return matches;
}

} // namespace snowseek::query
