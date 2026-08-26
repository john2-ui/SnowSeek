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
        /**
         * @brief Creates an error identifying invalid UTF-8 in a source file.
         * @param path Source file containing the invalid sequence.
         * @param byte_offset Zero-based source byte offset of the sequence.
         */
        InvalidUtf8Error(std::filesystem::path path, std::uint64_t byte_offset);

        /** @brief Returns the source path associated with the error. */
        [[nodiscard]] const std::filesystem::path &path() const noexcept;

        /** @brief Returns the zero-based source byte offset of the error. */
        [[nodiscard]] std::uint64_t byte_offset() const noexcept;

      private:
        std::filesystem::path path_;
        std::uint64_t byte_offset_{};
};

// The supplied string_view remains valid only for the duration of the call.
using TextChunkConsumer = std::function<void(std::string_view)>;
using SourceChunkConsumer = std::function<void(std::string_view)>;

class TextReader {
      public:
        /**
         * @brief Creates a streaming UTF-8 text reader.
         * @param options Chunk size and invalid-sequence policy.
         * @throws std::invalid_argument If the chunk size cannot be used by the
         * underlying stream API.
         */
        explicit TextReader(TextReadOptions options = {});

        /**
         * @brief Reads a file and emits chunks ending on UTF-8 boundaries.
         * @param path Text file to read in binary mode.
         * @param consumer Callback invoked with each nonempty output chunk; its
         * string_view is valid only for the duration of the callback.
         * @param source_consumer Optional callback observing each raw input
         * chunk before UTF-8 decoding; its view has callback-only lifetime.
         * @return Source, output, and invalid-sequence byte statistics.
         * @throws std::invalid_argument If consumer is empty.
         * @throws std::runtime_error If the file cannot be opened or read.
         * @throws InvalidUtf8Error If strict mode encounters invalid UTF-8.
         */
        [[nodiscard]] TextReadStats
        read(const std::filesystem::path &path,
             const TextChunkConsumer &consumer,
             const SourceChunkConsumer &source_consumer = {}) const;

      private:
        TextReadOptions options_;
};

} // namespace snowseek::document
