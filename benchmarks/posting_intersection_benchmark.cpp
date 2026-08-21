#include "snowseek/query/posting_operations.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

/**
 * @brief Builds an ordered synthetic posting list with a fixed DocumentId step.
 * @param count Number of postings to generate.
 * @param step Distance between adjacent DocumentIds.
 * @return Posting list containing count entries in ascending order.
 */
[[nodiscard]] snowseek::index::PostingList
make_postings(std::size_t count, snowseek::document::DocumentId step) {
        snowseek::index::PostingList postings;
        postings.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
                const auto document_id =
                        static_cast<snowseek::document::DocumentId>(
                                index * static_cast<std::size_t>(step));
                postings.push_back({document_id, {0}});
        }
        return postings;
}

} // namespace

/**
 * @brief Measures linear intersection of two fixed ordered posting lists.
 * @return Zero after printing input sizes, match count, and elapsed time.
 */
int main() {
        constexpr std::size_t posting_count = 200'000;
        const auto left = make_postings(posting_count, 2);
        const auto right = make_postings(posting_count, 3);

        const auto started_at = std::chrono::steady_clock::now();
        const auto matches =
                snowseek::query::intersect_document_ids(left, right);
        const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started_at);

        std::cout << "left=" << left.size() << " right=" << right.size()
                  << " matches=" << matches.size()
                  << " elapsed_us=" << elapsed.count() << '\n';
        return 0;
}
