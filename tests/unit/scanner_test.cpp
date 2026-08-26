/**
 * @file scanner_test.cpp
 * @brief Verifies recursive filesystem scanning, limits, and diagnostic
 * handling.
 */

#include "filesystem/scanner.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

class TemporaryDirectory {
      public:
        /** @brief Creates a unique temporary directory for one test scope. */
        TemporaryDirectory() {
                const auto seed = std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count();
                const auto base = std::filesystem::temp_directory_path();
                for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
                        path_ = base /
                                ("snowseek-test-" + std::to_string(seed) + "-" +
                                 std::to_string(attempt));
                        std::error_code error;
                        if (std::filesystem::create_directory(path_, error)) {
                                return;
                        }
                }
                throw std::runtime_error(
                        "failed to create a temporary test directory");
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;
        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        /** @brief Removes the temporary directory and its contents. */
        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        /** @brief Returns the temporary directory path. */
        [[nodiscard]] const std::filesystem::path &path() const {
                return path_;
        }

      private:
        std::filesystem::path path_; ///< Root directory for scanner fixtures.
};

/**
 * @brief Writes binary fixture contents to a path.
 * @param path Destination fixture path.
 * @param contents Bytes to write.
 * @throws std::runtime_error If the fixture cannot be written.
 */
void write_file(const std::filesystem::path &path, std::string_view contents) {
        std::ofstream output(path, std::ios::binary);
        output << contents;
        if (!output) {
                throw std::runtime_error("failed to write scanner test file");
        }
}

bool contains(const std::vector<std::filesystem::path> &paths,
              const std::filesystem::path &path) {
        for (const auto &candidate : paths) {
                if (candidate == path) {
                        return true;
                }
        }
        return false;
}

/** @brief Verifies recursive discovery of regular files. */
void finds_regular_files_recursively() {
        const TemporaryDirectory temporary;
        const auto nested = temporary.path() / "nested";
        std::filesystem::create_directory(nested);
        const auto first = temporary.path() / "first.txt";
        const auto second = nested / "second.cpp";
        write_file(first, "first");
        write_file(second, "second");

        const auto result =
                snowseek::filesystem::Scanner{}.scan(temporary.path());
        snowseek::test::require(result.errors.empty(),
                                "a valid directory should not produce errors");
        snowseek::test::require_equal(result.files.size(), std::size_t{2},
                                      "scanner should find both regular files");
        snowseek::test::require(contains(result.files, first),
                                "scanner should include the root file");
        snowseek::test::require(contains(result.files, second),
                                "scanner should include the nested file");
}

/** @brief Verifies filtering by the maximum file size. */
void enforces_file_size_limit() {
        const TemporaryDirectory temporary;
        const auto small = temporary.path() / "small.txt";
        const auto large = temporary.path() / "large.txt";
        write_file(small, "1234");
        write_file(large, "12345");

        snowseek::filesystem::ScanOptions options;
        options.max_file_size = 4;
        const auto result =
                snowseek::filesystem::Scanner{options}.scan(temporary.path());
        snowseek::test::require(result.errors.empty(),
                                "size filtering should not produce errors");
        snowseek::test::require_equal(result.files.size(), std::size_t{1},
                                      "oversized files should be excluded");
        snowseek::test::require(contains(result.files, small),
                                "a file at the size limit should be included");
}

/** @brief Verifies diagnostics for a missing scan root. */
void reports_missing_root() {
        const TemporaryDirectory temporary;
        const auto missing = temporary.path() / "missing";
        const auto result = snowseek::filesystem::Scanner{}.scan(missing);

        snowseek::test::require(result.files.empty(),
                                "a missing root should produce no files");
        snowseek::test::require_equal(
                result.errors.size(), std::size_t{1},
                "a missing root should produce one error");
        snowseek::test::require_equal(
                result.errors[0].path, missing,
                "the error should identify the missing root");
        snowseek::test::require_equal(
                result.errors[0].error,
                std::make_error_code(std::errc::no_such_file_or_directory),
                "the error should report a missing path");
}

/** @brief Verifies include and exclude pattern composition. */
void applies_include_and_exclude_patterns() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "src";
        const auto generated = temporary.path() / "generated";
        std::filesystem::create_directory(source);
        std::filesystem::create_directory(generated);

        const auto implementation = source / "main.cpp";
        const auto header = source / "main.hpp";
        const auto generated_source = generated / "auto.cpp";
        const auto documentation = temporary.path() / "README.md";
        write_file(implementation, "int main() {}");
        write_file(header, "#pragma once");
        write_file(generated_source, "generated");
        write_file(documentation, "documentation");

        snowseek::filesystem::ScanOptions options;
        options.include_patterns = {"*.cpp", "*.hpp"};
        options.exclude_patterns = {"generated/*"};
        const auto result =
                snowseek::filesystem::Scanner{options}.scan(temporary.path());

        snowseek::test::require(result.errors.empty(),
                                "pattern filtering should not produce errors");
        snowseek::test::require_equal(
                result.files.size(), std::size_t{2},
                "only included non-generated files should remain");
        snowseek::test::require(contains(result.files, implementation),
                                "the implementation file should be included");
        snowseek::test::require(contains(result.files, header),
                                "the header file should be included");
        snowseek::test::require(
                !contains(result.files, generated_source),
                "the excluded generated file should not be included");
        snowseek::test::require(
                !contains(result.files, documentation),
                "files outside include patterns should not be included");
}

/** @brief Verifies deterministic path ordering in scan results. */
void returns_files_in_stable_order() {
        const TemporaryDirectory temporary;
        const auto last = temporary.path() / "z-last.txt";
        const auto first = temporary.path() / "a-first.txt";
        const auto middle = temporary.path() / "m-middle.txt";
        write_file(last, "last");
        write_file(first, "first");
        write_file(middle, "middle");

        const auto result =
                snowseek::filesystem::Scanner{}.scan(temporary.path());
        const std::vector<std::filesystem::path> expected{first, middle, last};

        snowseek::test::require(result.errors.empty(),
                                "sorting should not produce errors");
        snowseek::test::require_equal(
                result.files, expected,
                "scanner output should use stable path ordering");
}

/** @brief Verifies optional traversal of file symbolic links. */
void respects_file_symlink_option() {
        const TemporaryDirectory temporary;
        const auto target = temporary.path() / "target.txt";
        const auto link = temporary.path() / "link.txt";
        write_file(target, "target");

        std::error_code error;
        std::filesystem::create_symlink(target, link, error);
        snowseek::test::require(!error,
                                "the test should be able to create a symlink");

        const auto default_result =
                snowseek::filesystem::Scanner{}.scan(temporary.path());
        snowseek::test::require_equal(default_result.files.size(),
                                      std::size_t{1},
                                      "symlinks should be skipped by default");
        snowseek::test::require(!contains(default_result.files, link),
                                "the default scan should exclude a symlink");

        snowseek::filesystem::ScanOptions options;
        options.follow_symlinks = true;
        const auto followed_result =
                snowseek::filesystem::Scanner{options}.scan(temporary.path());
        snowseek::test::require_equal(
                followed_result.files.size(), std::size_t{2},
                "enabled symlink following should include a file symlink");
        snowseek::test::require(contains(followed_result.files, link),
                                "the followed symlink should be returned");
}

/** @brief Verifies cycle prevention when directory links are followed. */
void prevents_directory_symlink_cycles() {
        const TemporaryDirectory temporary;
        const auto nested = temporary.path() / "nested";
        std::filesystem::create_directory(nested);
        const auto file = nested / "file.txt";
        const auto cycle = nested / "root";
        write_file(file, "content");

        std::error_code error;
        std::filesystem::create_directory_symlink(temporary.path(), cycle,
                                                  error);
        snowseek::test::require(!error,
                                "the test should create a directory symlink");

        snowseek::filesystem::ScanOptions options;
        options.follow_symlinks = true;
        const auto result =
                snowseek::filesystem::Scanner{options}.scan(temporary.path());

        snowseek::test::require(result.errors.empty(),
                                "a symlink cycle should not produce errors");
        snowseek::test::require_equal(
                result.files.size(), std::size_t{1},
                "a physical file should only be visited once");
        snowseek::test::require(contains(result.files, file),
                                "the file inside the cycle should be found");
}

} // namespace

/** @brief Runs the filesystem-scanner unit-test suite. */
int main() {
        return snowseek::test::run({
                {"finds regular files recursively",
                 finds_regular_files_recursively},
                {"enforces the file size limit", enforces_file_size_limit},
                {"reports a missing root", reports_missing_root},
                {"applies include and exclude patterns",
                 applies_include_and_exclude_patterns},
                {"returns files in stable order",
                 returns_files_in_stable_order},
                {"respects the file symlink option",
                 respects_file_symlink_option},
                {"prevents directory symlink cycles",
                 prevents_directory_symlink_cycles},
        });
}
