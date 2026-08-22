#pragma once

#include <cstdint>
#include <string_view>

namespace snowseek::storage {

class Crc32c {
      public:
        /**
         * @brief Adds bytes to this incremental CRC32C calculation.
         * @param bytes Next contiguous byte range in logical input order;
         * embedded null bytes are included.
         */
        void update(std::string_view bytes) noexcept;

        /**
         * @brief Returns the CRC32C of all bytes supplied so far.
         * @return Finalized Castagnoli checksum without closing the object.
         */
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
