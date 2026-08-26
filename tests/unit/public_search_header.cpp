/**
 * @file public_search_header.cpp
 * @brief Checks that the public search header is self-contained and usable.
 */

#include "snowseek/search.hpp"

#include <type_traits>

namespace snowseek::test {

/** @brief Exercises optional snippets without relying on sentinel values. */
bool search_header_is_self_contained() {
        static_assert(!std::is_copy_constructible_v<Searcher>);
        static_assert(std::is_nothrow_move_constructible_v<Searcher>);
        SearchHit hit;
        if (hit.snippet.has_value()) {
                return false;
        }
        hit.snippet = SourceSnippet{.line = 1, .text = "match"};
        return hit.snippet->line == 1 && hit.snippet->text == "match";
}

} // namespace snowseek::test
