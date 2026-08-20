#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace snowseek::filesystem {

struct ScanOptions {
    std::uintmax_t max_file_size = 16U * 1024U * 1024U;
    bool follow_symlinks = false;
};

class Scanner {
public:
    explicit Scanner(ScanOptions options = {});
    [[nodiscard]] std::vector<std::filesystem::path> scan(
        const std::filesystem::path& root) const;

private:
    ScanOptions options_;
};

}  // namespace snowseek::filesystem

