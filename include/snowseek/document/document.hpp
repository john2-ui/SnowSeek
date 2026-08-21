#pragma once

#include <cstdint>
#include <filesystem>

namespace snowseek::document {

using DocumentId = std::uint32_t;

struct DocumentMeta {
        DocumentId id{};
        std::filesystem::path path;
        std::uint64_t file_size{};
        std::int64_t modified_time_ns{};
        std::uint32_t token_count{};
};

} // namespace snowseek::document
