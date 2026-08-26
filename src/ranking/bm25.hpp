#pragma once

#include <cstdint>

namespace snowseek::ranking {

/**
 * @brief Computes one BM25 term contribution for a document.
 * @param term_frequency Occurrences of the term in the document.
 * @param document_frequency Documents containing the term.
 * @param document_length Indexed token count for the document.
 * @param document_count Indexed document count for the corpus.
 * @param average_document_length Mean indexed token count in the corpus.
 * @param k1 Term-frequency saturation parameter.
 * @param b Document-length normalization parameter.
 * @return Nonnegative relevance contribution, or zero for absent terms and
 * empty corpora.
 */
[[nodiscard]] double
bm25(std::uint32_t term_frequency, std::uint32_t document_frequency,
     std::uint32_t document_length, std::uint32_t document_count,
     double average_document_length, double k1 = 1.2, double b = 0.75);

} // namespace snowseek::ranking
