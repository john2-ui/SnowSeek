#include "snowseek/filesystem/scanner.hpp"

#include <system_error>

namespace snowseek::filesystem {

Scanner::Scanner(ScanOptions options) : options_(options) {}

std::vector<std::filesystem::path> Scanner::scan(
    const std::filesystem::path& root) const {
    std::vector<std::filesystem::path> paths;
    const auto options = options_.follow_symlinks
                             ? std::filesystem::directory_options::follow_directory_symlink
                             : std::filesystem::directory_options::none;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(root, options, error), end;
         iterator != end; iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!iterator->is_regular_file(error)) {
            continue;
        }
        const auto size = iterator->file_size(error);
        if (!error && size <= options_.max_file_size) {
            paths.push_back(iterator->path());
        }
    }
    return paths;
}

}  // namespace snowseek::filesystem

