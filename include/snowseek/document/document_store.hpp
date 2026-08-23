#pragma once

#include "snowseek/document/document.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace snowseek::document {

class DocumentStore {
      public:
        /**
         * @brief Adds a document and assigns the next contiguous identifier.
         * @param path Path used to identify the document in the source corpus.
         * @param file_size Size of the source file in bytes.
         * @param modified_time_ns Modification time as Unix Epoch nanoseconds.
         * @return The identifier assigned to the stored document.
         * @throws std::overflow_error If no DocumentId remains available.
         */
        [[nodiscard]] DocumentId add(std::filesystem::path path,
                                     std::uint64_t file_size,
                                     std::int64_t modified_time_ns);

        /**
         * @brief Records the number of indexed tokens for a document.
         * @param id Identifier of the document to update.
         * @param token_count Number of indexed tokens in the document.
         * @throws std::out_of_range If id does not identify a stored document.
         */
        void set_token_count(DocumentId id, std::uint32_t token_count);

        /**
         * @brief Retrieves immutable metadata for a stored document.
         * @param id Identifier of the document to retrieve.
         * @return A reference valid until the store is modified or destroyed.
         * @throws std::out_of_range If id does not identify a stored document.
         */
        [[nodiscard]] const DocumentMeta &get(DocumentId id) const;

        /**
         * @brief Reports the number of stored documents.
         * @return The current document count.
         */
        [[nodiscard]] std::size_t size() const noexcept;

        /**
         * @brief Exposes document metadata in contiguous DocumentId order.
         * @return A read-only view valid until the store is modified.
         */
        [[nodiscard]] const std::vector<DocumentMeta> &all() const noexcept;

        /**
         * @brief Estimates dynamic storage retained by document metadata.
         * @return Conservative capacity-based bytes for the document vector and
         * path character storage.
         * @throws std::overflow_error If the estimate exceeds std::uint64_t.
         * @note Allocator metadata and runtime-library overhead are excluded.
         */
        [[nodiscard]] std::uint64_t estimated_memory_bytes() const;

      private:
        std::vector<DocumentMeta> documents_;
};

} // namespace snowseek::document
