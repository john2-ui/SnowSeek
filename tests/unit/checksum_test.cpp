#include "snowseek/storage/checksum.hpp"

#include "test_support.hpp"

#include <string_view>

namespace {

/** @brief Verifies the CRC32C definition for empty and standard inputs. */
void matches_crc32c_vectors() {
        snowseek::test::require_equal(snowseek::storage::crc32c({}),
                                      std::uint32_t{0},
                                      "empty CRC32C should be zero");
        snowseek::test::require_equal(
                snowseek::storage::crc32c("123456789"),
                std::uint32_t{0xe3069283U},
                "CRC32C should match the Castagnoli check vector");
}

/** @brief Verifies incremental updates equal a one-shot checksum. */
void supports_incremental_updates() {
        constexpr std::string_view text = "SnowSeek checksum fixture";
        snowseek::storage::Crc32c incremental;
        incremental.update(text.substr(0, 8));
        incremental.update(text.substr(8, 9));
        incremental.update(text.substr(17));

        snowseek::test::require_equal(
                incremental.value(), snowseek::storage::crc32c(text),
                "chunk boundaries should not change CRC32C");
}

} // namespace

/** @brief Runs the checksum unit-test suite. */
int main() {
        return snowseek::test::run({
                {"matches CRC32C vectors", matches_crc32c_vectors},
                {"supports incremental updates", supports_incremental_updates},
        });
}
