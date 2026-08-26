/**
 * @file scanner.hpp
 * @brief Declares recursive source-file scanning options and results.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace snowseek::filesystem {

struct ScanOptions {
        std::uintmax_t max_file_size = 16U * 1024U * 1024U; ///< Largest accepted file size in bytes.
        bool follow_symlinks = false; ///< Whether traversal follows links.
        std::vector<std::string> include_patterns; ///< Admission Glob patterns.
        std::vector<std::string> exclude_patterns; ///< Rejection Glob patterns.
};

struct ScanError {
        std::filesystem::path path; ///< Entry that could not be inspected.
        std::error_code error;      ///< Recoverable filesystem failure.
};

struct ScanResult {
        std::vector<std::filesystem::path> files; ///< Sorted eligible files.
        std::vector<ScanError> errors; ///< Sorted recoverable failures.
};

class Scanner {
      public:
        /**
         * @brief Creates a recursive scanner with filtering and link options.
         * @param options File size, pattern, and symbolic-link rules.
         */
        explicit Scanner(ScanOptions options = {});

        /**
         * @brief Recursively discovers eligible regular files below a root.
         * @param root Directory from which traversal begins.
         * @return Stably sorted files plus recoverable traversal errors.
         */
        [[nodiscard]] ScanResult scan(const std::filesystem::path &root) const;

      private:
        ScanOptions options_; ///< Rules applied by every scan call.
};

} // namespace snowseek::filesystem
