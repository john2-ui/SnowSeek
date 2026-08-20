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

        [[nodiscard]] std::uint32_t term_frequency() const;
};

using PostingList = std::vector<Posting>;

class InMemoryIndex {
      public:
        void add_occurrence(std::string_view term,
                            document::DocumentId document_id,
                            Position position);

        [[nodiscard]] const PostingList *find(std::string_view term) const;
        [[nodiscard]] std::size_t term_count() const noexcept;

      private:
        std::unordered_map<std::string, PostingList> dictionary_;
};

} // namespace snowseek::index
