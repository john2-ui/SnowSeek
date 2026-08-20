#include "snowseek/filesystem/scanner.hpp"

#include <algorithm>
#include <fnmatch.h>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace snowseek::filesystem {

namespace {
bool matches_pattern(std::string_view pattern,
                     const std::filesystem::path &relative_path) {
        const bool matches_relative_path =
                pattern.find('/') != std::string_view::npos;

        const std::string candidate =
                matches_relative_path
                        ? relative_path.generic_string()
                        : relative_path.filename().generic_string();

        const std::string owned_pattern(pattern);

        return ::fnmatch(owned_pattern.c_str(), candidate.c_str(), 0) == 0;
}

bool matches_any(const std::vector<std::string> &patterns,
                 const std::filesystem::path &relative_path) {
        return std::any_of(patterns.begin(), patterns.end(),
                           [&relative_path](const std::string &pattern) {
                                   return matches_pattern(pattern,
                                                          relative_path);
                           });
}

bool should_include(const ScanOptions &options,
                    const std::filesystem::path &relative_path) {
        if (!options.include_patterns.empty() &&
            !matches_any(options.include_patterns, relative_path)) {
                return false;
        }

        if (matches_any(options.exclude_patterns, relative_path)) {
                return false;
        }
        return true;
}

void append_error(ScanResult &result, const std::filesystem::path &path,
                  const std::error_code &error) {
        result.errors.push_back(ScanError{path, error});
}

void sort_result(ScanResult &result) {
        std::sort(result.files.begin(), result.files.end());
        std::sort(result.errors.begin(), result.errors.end(),
                  [](const ScanError &left, const ScanError &right) {
                          if (left.path != right.path) {
                                  return left.path < right.path;
                          }
                          return left.error.value() < right.error.value();
                  });
}

} // namespace

Scanner::Scanner(ScanOptions options) : options_(std::move(options)) {}

ScanResult Scanner::scan(const std::filesystem::path &root) const {
        ScanResult result;

        std::error_code error;
        const auto root_status = std::filesystem::status(root, error);

        if (error) {
                append_error(result, root, error);
                return result;
        }

        if (!std::filesystem::exists(root_status)) {
                append_error(result, root,
                             std::make_error_code(
                                     std::errc::no_such_file_or_directory));
                return result;
        }

        if (!std::filesystem::is_directory(root_status)) {
                append_error(result, root,
                             std::make_error_code(std::errc::not_a_directory));
                return result;
        }

        std::set<std::filesystem::path> visited_directories;
        if (options_.follow_symlinks) {
                const auto canonical_root =
                        std::filesystem::canonical(root, error);
                if (error) {
                        append_error(result, root, error);
                        return result;
                }
                visited_directories.insert(canonical_root);
        }

        const auto directory_options =
                options_.follow_symlinks
                        ? std::filesystem::directory_options::
                                  follow_directory_symlink
                        : std::filesystem::directory_options::none;

        std::filesystem::recursive_directory_iterator iterator(
                root, directory_options, error);
        const std::filesystem::recursive_directory_iterator end;

        if (error) {
                append_error(result, root, error);
                return result;
        }

        while (iterator != end) {
                const auto path = iterator->path();

                std::error_code entry_error;
                const bool is_symlink = iterator->is_symlink(entry_error);
                if (entry_error) {
                        append_error(result, path, entry_error);
                } else if (options_.follow_symlinks || !is_symlink) {
                        if (options_.follow_symlinks &&
                            iterator->is_directory(entry_error)) {
                                const auto canonical_path =
                                        std::filesystem::canonical(path,
                                                                   entry_error);
                                if (entry_error) {
                                        append_error(result, path, entry_error);
                                        iterator.disable_recursion_pending();
                                } else if (!visited_directories
                                                    .insert(canonical_path)
                                                    .second) {
                                        iterator.disable_recursion_pending();
                                }
                        }

                        if (entry_error) {
                                error.clear();
                                iterator.increment(error);
                                if (error) {
                                        append_error(result, path, error);
                                }
                                continue;
                        }

                        const bool is_regular_file =
                                iterator->is_regular_file(entry_error);

                        if (entry_error) {
                                append_error(result, path, entry_error);
                        } else if (is_regular_file) {
                                const auto file_size =
                                        iterator->file_size(entry_error);
                                if (entry_error) {
                                        append_error(result, path, entry_error);
                                } else if (file_size <=
                                           options_.max_file_size) {
                                        const auto relative_path =
                                                path.lexically_relative(root);

                                        if (should_include(options_,
                                                           relative_path)) {
                                                result.files.push_back(path);
                                        }
                                }
                        }
                }

                error.clear();
                iterator.increment(error);

                if (error) {
                        append_error(result, path, error);
                        error.clear();
                }
        }

        sort_result(result);
        return result;
}

} // namespace snowseek::filesystem
