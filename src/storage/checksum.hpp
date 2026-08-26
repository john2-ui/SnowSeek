#pragma once

#include <cstdint>
#include <string_view>

namespace snowseek::storage {

class Crc32c {
      public:
        /** @brief Creates an empty CRC32C calculation. */
        Crc32c() noexcept = default;

        /**
         * @brief Extends the checksum with the next contiguous byte range.
         * @param bytes Bytes to append, including embedded nulls.
         */
        void update(std::string_view bytes) noexcept;

        /** @brief Returns the checksum of all bytes supplied so far. */
        [[nodiscard]] std::uint32_t value() const noexcept;

      private:
        std::uint32_t state_ = 0xffffffffU;
};

/**
 * @brief Computes the CRC32C Castagnoli checksum of a byte range.
 * @param bytes Complete byte range to checksum, including embedded nulls.
 * @return CRC32C using an all-ones initial state and final XOR.
 */
[[nodiscard]] std::uint32_t crc32c(std::string_view bytes) noexcept;

} // namespace snowseek::storage
