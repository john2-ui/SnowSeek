#pragma once

#include <cstdint>

namespace snowseek::ranking {

/**
 * @brief Computes a BM25 contribution for one term in one document.
 * @param term_frequency Occurrences of the term in the document.
 * @param document_frequency Number of documents containing the term.
 * @param document_length Number of indexed tokens in the document.
 * @param document_count Number of documents in the corpus.
 * @param average_document_length Mean indexed document length in the corpus.
 * @param k1 Term-frequency saturation parameter.
 * @param b Document-length normalization parameter.
 * @return The term score, or zero for incomplete corpus statistics.
 */
[[nodiscard]] double
bm25(std::uint32_t term_frequency, std::uint32_t document_frequency,
     std::uint32_t document_length, std::uint32_t document_count,
     double average_document_length, double k1 = 1.2, double b = 0.75);

} // namespace snowseek::ranking
