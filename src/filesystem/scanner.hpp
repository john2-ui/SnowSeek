#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace snowseek::filesystem {

struct ScanOptions {
        std::uintmax_t max_file_size = 16U * 1024U * 1024U; // 16 MB
        bool follow_symlinks = false;
        std::vector<std::string> include_patterns;
        std::vector<std::string> exclude_patterns;
};

struct ScanError {
        std::filesystem::path path;
        std::error_code error;
};

struct ScanResult {
        std::vector<std::filesystem::path> files;
        std::vector<ScanError> errors;
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
        ScanOptions options_;
};

} // namespace snowseek::filesystem
