#include "snowseek/storage/checksum.hpp"

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

} // namespace

/** @brief Runs the checksum unit-test suite. */
int main() {
        return snowseek::test::run({
                {"matches CRC32C vectors", matches_crc32c_vectors},
        });
}
