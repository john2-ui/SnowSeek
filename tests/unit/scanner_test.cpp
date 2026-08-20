#include "snowseek/filesystem/scanner.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

class TemporaryDirectory {
      public:
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

        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        [[nodiscard]] const std::filesystem::path &path() const {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

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

void finds_regular_files_recursively() {
        const TemporaryDirectory temporary;
        const auto nested = temporary.path() / "nested";
        std::filesystem::create_directory(nested);
        const auto first = temporary.path() / "first.txt";
        const auto second = nested / "second.cpp";
        write_file(first, "first");
        write_file(second, "second");

        const auto paths =
                snowseek::filesystem::Scanner{}.scan(temporary.path());
        snowseek::test::require_equal(paths.size(), std::size_t{2},
                                      "scanner should find both regular files");
        snowseek::test::require(contains(paths, first),
                                "scanner should include the root file");
        snowseek::test::require(contains(paths, second),
                                "scanner should include the nested file");
}

void enforces_file_size_limit() {
        const TemporaryDirectory temporary;
        const auto small = temporary.path() / "small.txt";
        const auto large = temporary.path() / "large.txt";
        write_file(small, "1234");
        write_file(large, "12345");

        snowseek::filesystem::ScanOptions options;
        options.max_file_size = 4;
        const auto paths =
                snowseek::filesystem::Scanner{options}.scan(temporary.path());
        snowseek::test::require_equal(paths.size(), std::size_t{1},
                                      "oversized files should be excluded");
        snowseek::test::require(contains(paths, small),
                                "a file at the size limit should be included");
}

void handles_missing_root() {
        const TemporaryDirectory temporary;
        const auto missing = temporary.path() / "missing";
        const auto paths = snowseek::filesystem::Scanner{}.scan(missing);
        snowseek::test::require(paths.empty(),
                                "a missing root should produce no paths");
}

} // namespace

int main() {
        return snowseek::test::run({
                {"finds regular files recursively",
                 finds_regular_files_recursively},
                {"enforces the file size limit", enforces_file_size_limit},
                {"handles a missing root", handles_missing_root},
        });
}
