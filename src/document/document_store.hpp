/**
 * @file document_store.hpp
 * @brief Declares the in-memory table of indexed document metadata.
 */

#pragma once

#include "document/document.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace snowseek::document {

class DocumentStore {
      public:
        /**
         * @brief Adds a document and assigns the next contiguous identifier.
         * @param path Path used to identify the document in the source corpus.
         * @param file_size Size of the source file in bytes.
         * @param modified_time_ns Modification time as Unix Epoch nanoseconds.
         * @param content_crc32c Optional raw source fingerprint; absent for
         * legacy or otherwise unavailable metadata.
         * @return The identifier assigned to the stored document.
         * @throws std::overflow_error If no DocumentId remains available.
         */
        [[nodiscard]] DocumentId add(std::filesystem::path path,
                                     std::uint64_t file_size,
                                     std::int64_t modified_time_ns,
                                     std::optional<std::uint32_t>
                                             content_crc32c = std::nullopt);

        /**
         * @brief Adds a path deletion marker with no indexed content.
         * @param path Relative document path invalidated by the marker.
         * @return The contiguous identifier assigned to the Tombstone.
         * @throws std::overflow_error If no DocumentId remains available.
         */
        [[nodiscard]] DocumentId
        add_tombstone(std::filesystem::path path);

        /**
         * @brief Records the number of indexed tokens for a document.
         * @param id Identifier of the document to update.
         * @param token_count Number of indexed tokens in the document.
         * @throws std::out_of_range If id does not identify a stored document.
         * @throws std::invalid_argument If a Tombstone receives a nonzero
         * count.
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
        std::vector<DocumentMeta> documents_; ///< Records in DocumentId order.
};

} // namespace snowseek::document
