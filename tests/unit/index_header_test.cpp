#include "snowseek/storage/index_header.hpp"

#include "test_support.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

namespace {

/** @brief Verifies index-header serialization round trips. */
void round_trips_header() {
        std::stringstream stream(std::ios::in | std::ios::out |
                                 std::ios::binary);
        const snowseek::storage::IndexHeader expected{1, 7};
        snowseek::storage::write_header(stream, expected);
        stream.seekg(0);
        const auto actual = snowseek::storage::read_header(stream);
        snowseek::test::require_equal(actual.version, expected.version,
                                      "index version should round-trip");
        snowseek::test::require_equal(actual.feature_flags,
                                      expected.feature_flags,
                                      "index feature flags should round-trip");
}

/** @brief Verifies rejection of a header with invalid magic bytes. */
void rejects_invalid_magic() {
        std::stringstream stream("NOT-AN-INDEX",
                                 std::ios::in | std::ios::binary);
        snowseek::test::require_throws<std::runtime_error>(
                [&stream] {
                        static_cast<void>(
                                snowseek::storage::read_header(stream));
                },
                "invalid index magic should be rejected");
}

/** @brief Verifies rejection of an incomplete serialized header. */
void rejects_truncated_header() {
        std::string bytes(snowseek::storage::kIndexMagic.begin(),
                          snowseek::storage::kIndexMagic.end());
        bytes.push_back('\1');
        std::stringstream stream(bytes, std::ios::in | std::ios::binary);
        snowseek::test::require_throws<std::runtime_error>(
                [&stream] {
                        static_cast<void>(
                                snowseek::storage::read_header(stream));
                },
                "a truncated header should be rejected");
}

} // namespace

/** @brief Runs the index-header unit-test suite. */
int main() {
        return snowseek::test::run({
                {"round-trips the index header", round_trips_header},
                {"rejects invalid index magic", rejects_invalid_magic},
                {"rejects a truncated header", rejects_truncated_header},
        });
}
