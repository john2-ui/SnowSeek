#pragma once

#include <cstddef>
#include <filesystem>

namespace snowseek::index {

struct BuildOptions {
    std::size_t memory_limit_bytes = 64U * 1024U * 1024U;
    unsigned int threads = 1;
    bool store_positions = true;
};

class IndexBuilder {
public:
    explicit IndexBuilder(BuildOptions options = {});
    void build(const std::filesystem::path& source,
               const std::filesystem::path& index_directory) const;

private:
    BuildOptions options_;
};

}  // namespace snowseek::index

