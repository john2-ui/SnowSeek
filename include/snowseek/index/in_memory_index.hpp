#pragma once

#include "snowseek/document/document.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowseek::index {

using Position = std::uint32_t;

struct Posting {
        document::DocumentId document_id{};
        std::vector<Position> positions;

        /**
         * @brief Reports how often the term occurs in this document.
         * @return The number of recorded positions.
         * @throws std::overflow_error If the count exceeds std::uint32_t.
         */
        [[nodiscard]] std::uint32_t term_frequency() const;
};

using PostingList = std::vector<Posting>;

class InMemoryIndex {
      public:
        /**
         * @brief Adds one ordered occurrence to a term's posting list.
         * @param term Nonempty normalized term to index.
         * @param document_id Document containing the occurrence; IDs for one
         * term must be nondecreasing.
         * @param position Position within the document; positions for one
         * posting must be strictly increasing.
         * @throws std::invalid_argument If the term is empty or ordering is
         * invalid.
         * @throws std::overflow_error If term frequency exceeds std::uint32_t.
         */
        void add_occurrence(std::string_view term,
                            document::DocumentId document_id,
                            Position position);

        /**
         * @brief Finds the posting list for an exact normalized term.
         * @param term Term to look up without modifying the dictionary.
         * @return A pointer to the posting list, or nullptr when absent; the
         * pointer remains valid only while the index is not modified.
         */
        [[nodiscard]] const PostingList *find(std::string_view term) const;

        /** @brief Returns the number of distinct indexed terms. */
        [[nodiscard]] std::size_t term_count() const noexcept;

      private:
        std::unordered_map<std::string, PostingList> dictionary_;
};

} // namespace snowseek::index
