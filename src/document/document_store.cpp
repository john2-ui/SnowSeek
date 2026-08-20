#include "snowseek/document/document_store.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace snowseek::document {

DocumentId DocumentStore::add(std::filesystem::path path,
                              std::uint64_t file_size,
                              std::uint64_t modified_time) {
        if (documents_.size() > std::numeric_limits<DocumentId>::max()) {
                throw std::overflow_error("document id exceeds uint32_t");
        }

        const auto id = static_cast<DocumentId>(documents_.size());
        documents_.push_back(
                DocumentMeta{id, std::move(path), file_size, modified_time, 0});
        return id;
}

void DocumentStore::set_token_count(DocumentId id, std::uint32_t token_count) {
        if (id >= documents_.size()) {
                throw std::out_of_range("document id is out of range");
        }
        documents_[id].token_count = token_count;
}

const DocumentMeta &DocumentStore::get(DocumentId id) const {
        if (id >= documents_.size()) {
                throw std::out_of_range("document id is out of range");
        }
        return documents_[id];
}

std::size_t DocumentStore::size() const noexcept { return documents_.size(); }

} // namespace snowseek::document
