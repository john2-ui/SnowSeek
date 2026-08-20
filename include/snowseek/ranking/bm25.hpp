#pragma once

#include <cstdint>

namespace snowseek::ranking {

[[nodiscard]] double
bm25(std::uint32_t term_frequency, std::uint32_t document_frequency,
     std::uint32_t document_length, std::uint32_t document_count,
     double average_document_length, double k1 = 1.2, double b = 0.75);

} // namespace snowseek::ranking
