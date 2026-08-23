#include "snowseek/ranking/bm25.hpp"

#include <cmath>

namespace snowseek::ranking {

double bm25(std::uint32_t term_frequency, std::uint32_t document_frequency,
            std::uint32_t document_length, std::uint32_t document_count,
            double average_document_length, double k1, double b) {
        if (term_frequency == 0 || document_frequency == 0 ||
            document_count == 0 || average_document_length <= 0.0) {
                return 0.0;
        }
        const double idf = std::log(
                1.0 + (static_cast<double>(document_count) -
                       static_cast<double>(document_frequency) + 0.5) /
                              (static_cast<double>(document_frequency) + 0.5));
        const double frequency = static_cast<double>(term_frequency);
        const double normalization =
                k1 * (1.0 - b +
                      b * static_cast<double>(document_length) /
                              average_document_length);
        return idf * frequency * (k1 + 1.0) /
               (frequency + normalization);
}

} // namespace snowseek::ranking
