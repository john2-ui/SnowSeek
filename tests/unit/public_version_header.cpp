/**
 * @file public_version_header.cpp
 * @brief Checks that the public version header is self-contained and accurate.
 */

#include "snowseek/version.hpp"

#include <string_view>

namespace snowseek::test {

/** @brief Checks the public semantic version constant. */
bool version_header_is_self_contained() {
        return std::string_view(kVersion) == "0.2.0";
}

} // namespace snowseek::test
