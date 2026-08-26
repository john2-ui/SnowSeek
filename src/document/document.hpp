/**
 * @file document.hpp
 * @brief Defines indexed-document identifiers, state, and metadata.
 */

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
        DocumentId id{}; ///< Contiguous identifier in its containing store.
        std::filesystem::path path;    ///< Source path represented by the record.
        std::uint64_t file_size{};     ///< Source length in bytes.
        std::int64_t modified_time_ns{}; ///< Unix Epoch modification nanoseconds.
        std::uint32_t token_count{};   ///< Indexed tokens; zero for Tombstones.
        DocumentState state{DocumentState::live}; ///< Record visibility effect.
        std::optional<std::uint32_t>
                content_crc32c; ///< Raw CRC32C; absent for legacy/Tombstones.
};

} // namespace snowseek::document
