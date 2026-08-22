#include "snowseek/cli/application.hpp"

#include "snowseek/storage/index_file.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
      public:
        /** @brief Creates a unique root for CLI fixtures. */
        TemporaryDirectory() {
                const auto seed = std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count();
                path_ = std::filesystem::temp_directory_path() /
                        ("snowseek-cli-test-" + std::to_string(seed));
                std::filesystem::create_directory(path_);
        }

        /** @brief Removes all CLI fixtures. */
        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        /** @brief Returns the root fixture path. */
        [[nodiscard]] const std::filesystem::path &path() const {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

/** @brief Invokes the CLI entry point with writable argument pointers. */
int invoke(std::vector<std::string> arguments) {
        std::vector<char *> pointers;
        pointers.reserve(arguments.size());
        for (auto &argument : arguments) {
                pointers.push_back(argument.data());
        }
        return snowseek::cli::run(static_cast<int>(pointers.size()),
                                  pointers.data());
}

/** @brief Writes a CLI corpus fixture. */
void write_file(const std::filesystem::path &path, const std::string &contents) {
        std::ofstream output(path, std::ios::binary);
        output << contents;
        if (!output) {
                throw std::runtime_error("failed to write CLI fixture");
        }
}

/** @brief Verifies all M2 commands complete against one persisted index. */
void runs_index_query_stats_and_verify() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "timeout retry");

        snowseek::test::require_equal(
                invoke({"snowseek", "index", source.string(), "--index",
                        destination.string()}),
                0, "index command should succeed");
        snowseek::test::require(
                std::filesystem::exists(destination /
                                        snowseek::storage::kSegmentFileName),
                "index command should publish the Segment");
        snowseek::test::require_equal(
                invoke({"snowseek", "query", destination.string(),
                        "timeout"}),
                0, "query command should succeed");
        snowseek::test::require_equal(
                invoke({"snowseek", "stats", destination.string()}), 0,
                "stats command should succeed");
        snowseek::test::require_equal(
                invoke({"snowseek", "verify", destination.string()}), 0,
                "verify command should succeed");
}

/** @brief Verifies invalid syntax and partial indexes return nonzero status. */
void reports_cli_failures() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "good.txt", "safe");
        write_file(source / "bad.txt", std::string(300, 'x'));
        snowseek::test::require_equal(
                invoke({"snowseek", "index", source.string(), "--index",
                        destination.string()}),
                2, "partial index command should return two");
        snowseek::test::require_equal(
                invoke({"snowseek", "query", destination.string(),
                        "one OR two"}),
                1, "unsupported query grammar should fail");
        snowseek::test::require_equal(invoke({"snowseek", "unknown"}), 1,
                                      "unknown commands should fail");
}

} // namespace

/** @brief Runs the M2 CLI integration-test suite. */
int main() {
        return snowseek::test::run({
                {"runs index, query, stats, and verify",
                 runs_index_query_stats_and_verify},
                {"reports CLI failures", reports_cli_failures},
        });
}
