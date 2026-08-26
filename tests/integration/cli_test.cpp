#include "snowseek/cli/application.hpp"

#include "snowseek/storage/index_file.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct InvocationResult {
        int status{};
        std::string output;
        std::string error;
};

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

/**
 * @brief Invokes the CLI while capturing standard output and error.
 * @param arguments Writable command-line strings.
 * @return Exit status and captured streams.
 */
InvocationResult invoke_captured(std::vector<std::string> arguments) {
        std::ostringstream output;
        std::ostringstream error;
        auto *old_output = std::cout.rdbuf(output.rdbuf());
        auto *old_error = std::cerr.rdbuf(error.rdbuf());
        const int status = invoke(std::move(arguments));
        std::cout.rdbuf(old_output);
        std::cerr.rdbuf(old_error);
        return InvocationResult{status, output.str(), error.str()};
}

/** @brief Writes a CLI corpus fixture. */
void write_file(const std::filesystem::path &path,
                const std::string &contents) {
        std::ofstream output(path, std::ios::binary);
        output << contents;
        if (!output) {
                throw std::runtime_error("failed to write CLI fixture");
        }
}

/** @brief Verifies index, rich query, stats, and verify commands. */
void runs_index_query_stats_and_verify() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "timeout retry");

        const auto index =
                invoke_captured({"snowseek", "index", source.string(),
                                 "--index", destination.string()});
        snowseek::test::require_equal(index.status, 0,
                                      "index command should succeed");
        for (const char *key :
             {"memory_metadata_bytes=", "memory_reader_peak_bytes=",
              "segment_id=1", "manifest_generation=1",
              "memory_token_peak_bytes=", "memory_dictionary_bytes=",
              "memory_posting_bytes=", "memory_estimated_peak_bytes=",
              "resource_profile=balanced",
              "memory_limit_bytes=", "memory_peak_bytes=", "threads=2",
              "positions_enabled=1", "temporary_segment_count=",
              "temporary_peak_bytes=", "merge_pass_count="}) {
                snowseek::test::require(
                        index.output.find(key) != std::string::npos,
                        "index output should expose " + std::string(key));
        }
        snowseek::test::require(
                std::filesystem::exists(destination /
                                        snowseek::storage::kSegmentFileName),
                "index command should publish the Segment");
        const auto query = invoke_captured(
                {"snowseek", "query", destination.string(), "timeout",
                 "--source", source.string(), "--explain"});
        snowseek::test::require_equal(query.status, 0,
                                      "query command should succeed");
        snowseek::test::require(
                query.output.find("a.txt:1 score=") != std::string::npos &&
                        query.output.find("timeout tf=") != std::string::npos,
                "rich text should include snippet location and explanation");
        snowseek::test::require_equal(
                invoke({"snowseek", "stats", destination.string()}), 0,
                "stats command should succeed");
        snowseek::test::require_equal(
                invoke({"snowseek", "verify", destination.string()}), 0,
                "verify command should succeed");

        write_file(source / "a.txt", "replacement");
        const auto rebuilt =
                invoke_captured({"snowseek", "index", source.string(),
                                 "--index", destination.string()});
        snowseek::test::require(
                rebuilt.status == 0 &&
                        rebuilt.output.find("segment_id=2") !=
                                std::string::npos &&
                        rebuilt.output.find("manifest_generation=2") !=
                                std::string::npos,
                "a repeated CLI build should publish the next generation");
        snowseek::test::require(
                !std::filesystem::exists(destination /
                                         snowseek::storage::kSegmentFileName),
                "the second generation should clean the old ID 1 Segment");
        snowseek::test::require_equal(
                invoke({"snowseek", "stats", destination.string()}), 0,
                "stats should resolve the Manifest-selected Segment");
}

/** @brief Verifies index resource options and IEC size parsing. */
void parses_index_resource_options() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto first_destination = temporary.path() / "first-index";
        const auto second_destination = temporary.path() / "second-index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "alpha beta");

        const auto overridden = invoke_captured(
                {"snowseek", "index", source.string(), "--threads", "2",
                 "--merge-fan-in", "2", "--temporary-space-limit", "1MiB",
                 "--memory-limit", "1MiB", "--index",
                 first_destination.string(), "--profile", "minimal"});
        snowseek::test::require_equal(
                overridden.status, 0,
                "index options should allow arbitrary ordering and IEC sizes");
        snowseek::test::require(
                overridden.output.find("resource_profile=minimal") !=
                                std::string::npos &&
                        overridden.output.find("memory_limit_bytes=1048576") !=
                                std::string::npos &&
                        overridden.output.find("threads=2") !=
                                std::string::npos &&
                        overridden.output.find("positions_enabled=0") !=
                                std::string::npos,
                "explicit limits should override a profile independent of "
                "order");
        snowseek::test::require_equal(
                invoke({"snowseek", "index", source.string(), "--index",
                        second_destination.string(), "--temporary-space-limit",
                        "1048576"}),
                0, "a suffix-free temporary limit should mean bytes");
        for (const std::string_view limit :
             {"1048576B", "1024KiB", "1MiB", "1GiB", "1TiB"}) {
                snowseek::test::require_equal(
                        invoke({"snowseek", "index", source.string(), "--index",
                                second_destination.string(),
                                "--temporary-space-limit", std::string(limit)}),
                        0, "every documented size suffix should be accepted");
        }
        for (const std::string_view profile :
             {"minimal", "balanced", "performance"}) {
                snowseek::test::require_equal(
                        invoke({"snowseek", "index", source.string(), "--index",
                                second_destination.string(), "--profile",
                                std::string(profile)}),
                        0, "every resource profile should be accepted");
        }

        for (const std::vector<std::string> &arguments :
             {std::vector<std::string>{"snowseek", "index", source.string(),
                                       "--index", first_destination.string(),
                                       "--temporary-space-limit", "0"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--temporary-space-limit", "1MB"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--temporary-space-limit",
               "18446744073709551615TiB"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--merge-fan-in", "1"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--memory-limit", "0"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--threads", "0"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--profile", "Minimal"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--profile", "minimal", "--profile",
               "balanced"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--merge-fan-in", "2",
               "--merge-fan-in", "3"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--temporary-space-limit", "1MiB",
               "--temporary-space-limit", "2MiB"},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--temporary-space-limit"},
              {"snowseek", "index", source.string(), "--merge-fan-in", "2"}}) {
                snowseek::test::require_equal(
                        invoke(arguments), 1,
                        "invalid index resource options should be rejected");
        }

        const auto denied_destination = temporary.path() / "denied-index";
        snowseek::test::require_equal(
                invoke({"snowseek", "index", source.string(), "--index",
                        denied_destination.string(), "--temporary-space-limit",
                        "1B"}),
                1, "an insufficient CLI budget should fail before publication");
        snowseek::test::require_equal(
                invoke({"snowseek", "index", source.string(), "--index",
                        denied_destination.string(), "--memory-limit", "1B"}),
                1, "an insufficient memory budget should fail publication");
}

/** @brief Verifies JSONL and paths-only presentation modes. */
void renders_query_output_modes() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a\"quote.txt", "timeout retry");
        snowseek::test::require_equal(
                invoke({"snowseek", "index", source.string(), "--index",
                        destination.string()}),
                0, "index fixture should build");

        const auto json = invoke_captured(
                {"snowseek", "query", destination.string(), "timeout",
                 "--source", source.string(), "--jsonl", "--explain"});
        snowseek::test::require_equal(json.status, 0,
                                      "JSONL query should succeed");
        snowseek::test::require(
                json.output.find(
                        "{\"path\":\"a\\\"quote.txt\",\"line\":1,\"score\":") ==
                                0 &&
                        json.output.find("\"explanation\":[{") !=
                                std::string::npos,
                "JSONL should expose stable fields and explanation");

        const auto paths =
                invoke_captured({"snowseek", "query", destination.string(),
                                 "timeout", "--paths-only"});
        snowseek::test::require_equal(paths.status, 0,
                                      "paths-only query should succeed");
        snowseek::test::require_equal(paths.output,
                                      std::string("a\"quote.txt\n"),
                                      "paths-only output should remain stable");
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
                invoke({"snowseek", "query", destination.string(), "one two"}),
                1, "implicit AND should fail");
        snowseek::test::require_equal(
                invoke({"snowseek", "query", destination.string(), "safe",
                        "--jsonl", "--paths-only"}),
                1, "conflicting output modes should fail");
        snowseek::test::require_equal(
                invoke({"snowseek", "query", destination.string(), "safe",
                        "--paths-only", "--explain"}),
                1, "paths-only explanation should fail");
        snowseek::test::require_equal(
                invoke({"snowseek", "query", destination.string(), "safe",
                        "--top-k", "1001"}),
                1, "Top-K above the limit should fail");
        snowseek::test::require_equal(invoke({"snowseek", "unknown"}), 1,
                                      "unknown commands should fail");
}

} // namespace

/** @brief Runs the M3 CLI integration-test suite. */
int main() {
        return snowseek::test::run({
                {"runs index, query, stats, and verify",
                 runs_index_query_stats_and_verify},
                {"parses index resource options",
                 parses_index_resource_options},
                {"renders query output modes", renders_query_output_modes},
                {"reports CLI failures", reports_cli_failures},
        });
}
