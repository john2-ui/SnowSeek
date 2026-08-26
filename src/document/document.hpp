#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace snowseek::document {

using DocumentId = std::uint32_t;

/** @brief Logical effect of one immutable Segment document record. */
enum class DocumentState : std::uint32_t {
        live = 0,
        tombstone = 1,
};

struct DocumentMeta {
        DocumentId id{};
        std::filesystem::path path;
        std::uint64_t file_size{};
        std::int64_t modified_time_ns{};
        std::uint32_t token_count{};
        DocumentState state{DocumentState::live};
        std::optional<std::uint32_t> content_crc32c;
};

} // namespace snowseek::document
