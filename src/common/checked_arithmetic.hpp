#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace snowseek::common::detail {

/**
 * @brief Adds two counters without unsigned wraparound.
 * @param left First addend.
 * @param right Second addend.
 * @param field Counter name included in the overflow diagnostic.
 * @return Checked sum.
 * @throws std::overflow_error If the sum exceeds std::uint64_t.
 */
[[nodiscard]] inline std::uint64_t
checked_add(std::uint64_t left, std::uint64_t right, std::string_view field) {
        if (right > std::numeric_limits<std::uint64_t>::max() - left) {
                throw std::overflow_error("counter overflow: " +
                                          std::string(field));
        }
        return left + right;
}

/**
 * @brief Multiplies two counters without unsigned wraparound.
 * @param left First factor.
 * @param right Second factor.
 * @param field Counter name included in the overflow diagnostic.
 * @return Checked product.
 * @throws std::overflow_error If the product exceeds std::uint64_t.
 */
[[nodiscard]] inline std::uint64_t checked_multiply(std::uint64_t left,
                                                    std::uint64_t right,
                                                    std::string_view field) {
        if (left != 0 &&
            right > std::numeric_limits<std::uint64_t>::max() / left) {
                throw std::overflow_error("counter overflow: " +
                                          std::string(field));
        }
        return left * right;
}

} // namespace snowseek::common::detail
