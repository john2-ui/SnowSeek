#pragma once

#include "snowseek/document/document.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace snowseek::document {

class DocumentStore {
      public:
        [[nodiscard]] DocumentId add(std::filesystem::path path,
                                     std::uint64_t file_size,
                                     std::uint64_t modified_time);

        void set_token_count(DocumentId id, std::uint32_t token_count);

        [[nodiscard]] const DocumentMeta &get(DocumentId id) const;
        [[nodiscard]] std::size_t size() const noexcept;

      private:
        std::vector<DocumentMeta> documents_;
};

} // namespace snowseek::document
