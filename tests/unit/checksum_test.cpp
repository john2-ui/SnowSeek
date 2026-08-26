#include "storage/checksum.hpp"

#include "test_support.hpp"

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
void combines_incremental_crc32c_updates() {
        snowseek::storage::Crc32c checksum;
        checksum.update("123");
        checksum.update("");
        checksum.update("456789");
        snowseek::test::require_equal(
                checksum.value(), snowseek::storage::crc32c("123456789"),
                "chunked CRC32C should equal the one-shot result");
}

} // namespace

/** @brief Runs the checksum unit-test suite. */
int main() {
        return snowseek::test::run({
                {"matches CRC32C vectors", matches_crc32c_vectors},
                {"combines incremental CRC32C updates",
                 combines_incremental_crc32c_updates},
        });
}
