#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string_view>

namespace snowseek::document {

enum class InvalidUtf8Policy {
        replace,
        reject,
};

struct TextReadOptions {
        std::size_t chunk_size = 64U * 1024U;
        InvalidUtf8Policy invalid_utf8_policy = InvalidUtf8Policy::replace;
};

struct TextReadStats {
        std::uint64_t bytes_read{};
        std::uint64_t bytes_emitted{};
        std::uint64_t invalid_sequence_count{};
};

class InvalidUtf8Error final : public std::runtime_error {
      public:
        InvalidUtf8Error(std::filesystem::path path, std::uint64_t byte_offset);

        [[nodiscard]] const std::filesystem::path &path() const noexcept;
        [[nodiscard]] std::uint64_t byte_offset() const noexcept;

      private:
        std::filesystem::path path_;
        std::uint64_t byte_offset_{};
};

// The supplied string_view remains valid only for the duration of the call.
using TextChunkConsumer = std::function<void(std::string_view)>;

class TextReader {
      public:
        explicit TextReader(TextReadOptions options = {});

        [[nodiscard]] TextReadStats
        read(const std::filesystem::path &path,
             const TextChunkConsumer &consumer) const;

      private:
        TextReadOptions options_;
};

} // namespace snowseek::document
