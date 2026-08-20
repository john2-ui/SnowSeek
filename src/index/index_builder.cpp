#include "snowseek/index/index_builder.hpp"

#include <stdexcept>

namespace snowseek::index {

IndexBuilder::IndexBuilder(BuildOptions options) : options_(options) {
    if (options_.memory_limit_bytes == 0 || options_.threads == 0) {
        throw std::invalid_argument("memory limit and thread count must be non-zero");
    }
}

void IndexBuilder::build(const std::filesystem::path& source,
                         const std::filesystem::path& index_directory) const {
    if (!std::filesystem::exists(source)) {
        throw std::runtime_error("source path does not exist: " + source.string());
    }
    std::filesystem::create_directories(index_directory);
    // M1-M2: scanner, tokenizer and segment writer will be connected here.
}

}  // namespace snowseek::index

