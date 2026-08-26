#pragma once

#include "document/document.hpp"

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
        std::uint32_t frequency{};
        std::vector<Position> positions;

        /**
         * @brief Reports how often the term occurs in this document.
         * @return Stored nonzero frequency, independent of optional positions.
         */
        [[nodiscard]] std::uint32_t term_frequency() const;
};

using PostingList = std::vector<Posting>;

struct InMemoryIndexMemoryUsage {
        std::uint64_t dictionary_bytes{};
        std::uint64_t posting_bytes{};
};

class InMemoryIndex {
      public:
        /**
         * @brief Creates an index that optionally retains token positions.
         * @param store_positions Whether occurrences keep positional data.
         */
        explicit InMemoryIndex(bool store_positions = true) noexcept;

        InMemoryIndex(const InMemoryIndex &) = delete;
        InMemoryIndex &operator=(const InMemoryIndex &) = delete;
        InMemoryIndex(InMemoryIndex &&) noexcept = default;
        InMemoryIndex &operator=(InMemoryIndex &&) noexcept = default;

        /**
         * @brief Adds one ordered occurrence to a term's posting list.
         * @param term Nonempty normalized term to index.
         * @param document_id Document containing the occurrence; IDs for one
         * term must be nondecreasing.
         * @param position Position within the document; retained positions for
         * one posting must be strictly increasing.
         * @throws std::invalid_argument If the term is empty or ordering is
         * invalid.
         * @throws std::overflow_error If term frequency or the retained-memory
         * estimate exceeds its supported range.
         */
        void add_occurrence(std::string_view term,
                            document::DocumentId document_id,
                            Position position);

        /**
         * @brief Adds one complete ordered posting while loading an index.
         * @param term Nonempty normalized term to index.
         * @param document_id Document containing the term.
         * @param frequency Nonzero occurrence count retained for ranking.
         * @param positions Strictly increasing positions when enabled, or an
         * empty vector when disabled.
         * @throws std::invalid_argument If frequency, positions, or ordering
         * violate index invariants.
         * @throws std::overflow_error If retained-memory accounting overflows.
         */
        void add_posting(std::string_view term,
                         document::DocumentId document_id,
                         std::uint32_t frequency,
                         std::vector<Position> positions);

        /**
         * @brief Finds the posting list for an exact normalized term.
         * @param term Term to look up without modifying the dictionary.
         * @return A pointer to the posting list, or nullptr when absent; the
         * pointer remains valid only while the index is not modified.
         */
        [[nodiscard]] const PostingList *find(std::string_view term) const;

        /** @brief Returns the number of distinct indexed terms. */
        [[nodiscard]] std::size_t term_count() const noexcept;

        /**
         * @brief Returns all dictionary terms in deterministic byte order.
         * @return A sorted copy of the normalized term strings.
         */
        [[nodiscard]] std::vector<std::string> sorted_terms() const;

        /**
         * @brief Estimates dynamic storage retained by the in-memory index.
         * @return Conservative capacity-based dictionary and posting bytes.
         * @note Allocator metadata and runtime-library overhead are excluded.
         */
        [[nodiscard]] InMemoryIndexMemoryUsage estimated_memory_usage() const;

        /** @brief Returns whether this index retains token positions. */
        [[nodiscard]] bool stores_positions() const noexcept;

      private:
        /**
         * @brief Returns the posting list for a term, creating it if needed.
         * @param term Nonempty normalized dictionary key.
         * @return Mutable posting list owned by this index.
         * @throws std::invalid_argument If the term is empty.
         * @throws std::overflow_error If memory accounting overflows.
         */
        PostingList &postings_for(std::string_view term);

        /**
         * @brief Appends a posting and updates retained-memory accounting.
         * @param postings Ordered list that receives the posting.
         * @param posting Complete posting transferred into the list.
         * @throws std::overflow_error If memory accounting overflows.
         */
        void append_posting(PostingList &postings, Posting posting);

        std::unordered_map<std::string, PostingList> dictionary_;
        InMemoryIndexMemoryUsage memory_usage_;
        bool store_positions_{true};
};

} // namespace snowseek::index
