/**
 * @file cli_test.cpp
 * @brief Exercises CLI indexing, maintenance, search output, and error
 * handling.
 */

#include "cli/application.hpp"

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
        int status{};       ///< Exit status returned by the CLI entry point.
        std::string output; ///< Text captured from standard output.
        std::string error;  ///< Text captured from standard error.
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
        std::filesystem::path path_; ///< Root directory for CLI fixtures.
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

/** @brief Extracts key names from newline-delimited CLI maintenance output. */
[[nodiscard]] std::vector<std::string> output_keys(std::string_view output) {
        std::vector<std::string> keys;
        std::size_t begin = 0;
        while (begin < output.size()) {
                const auto end = output.find('\n', begin);
                const auto line =
                        output.substr(begin, end == std::string_view::npos
                                                     ? output.size() - begin
                                                     : end - begin);
                const auto separator = line.find('=');
                if (separator == std::string_view::npos) {
                        throw std::runtime_error(
                                "maintenance output is not key-value data");
                }
                keys.emplace_back(line.substr(0, separator));
                if (end == std::string_view::npos) {
                        break;
                }
                begin = end + 1;
        }
        return keys;
}

/** @brief Requires exactly the documented maintenance key order. */
void require_output_keys(const InvocationResult &result,
                         std::vector<std::string> expected) {
        snowseek::test::require_equal(
                output_keys(result.output), expected,
                "maintenance output keys should use the 0.2 order");
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
        require_output_keys(index, {"outcome", "revision", "segments",
                                    "indexed", "failed", "memory_peak_bytes",
                                    "temporary_peak_bytes", "warning_count"});
        snowseek::test::require(
                index.output.starts_with(
                        "outcome=published\nrevision=1\nsegments=1\nindexed=1\n"
                        "failed=0\n"),
                "index should report its first published revision");
        snowseek::test::require(
                std::filesystem::exists(destination /
                                        "segment-0000000000000001.idx"),
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
                        rebuilt.output.starts_with(
                                "outcome=published\nrevision=2\nsegments=1\n"),
                "a repeated CLI build should publish the next generation");
        snowseek::test::require(
                !std::filesystem::exists(destination /
                                         "segment-0000000000000001.idx"),
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
        const auto workspace_parent = temporary.path() / "workspace";
        const auto regular_file = temporary.path() / "regular-file";
        std::filesystem::create_directory(source);
        std::filesystem::create_directory(workspace_parent);
        write_file(source / "a.txt", "alpha beta");
        write_file(regular_file, "not a directory");

        const auto overridden = invoke_captured(
                {"snowseek", "index", source.string(), "--threads", "2",
                 "--merge-fan-in", "2", "--temporary-space-limit", "1MiB",
                 "--temporary-directory", workspace_parent.string(),
                 "--memory-limit", "1MiB", "--index",
                 first_destination.string(), "--profile", "minimal"});
        snowseek::test::require_equal(
                overridden.status, 0,
                "index options should allow arbitrary ordering and IEC sizes");
        require_output_keys(overridden,
                            {"outcome", "revision", "segments", "indexed",
                             "failed", "memory_peak_bytes",
                             "temporary_peak_bytes", "warning_count"});
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
               first_destination.string(), "--temporary-directory",
               workspace_parent.string(), "--temporary-directory",
               workspace_parent.string()},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--temporary-directory", ""},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--temporary-directory",
               (temporary.path() / "missing").string()},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--temporary-directory",
               regular_file.string()},
              {"snowseek", "index", source.string(), "--index",
               first_destination.string(), "--temporary-directory"},
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
        const auto partial =
                invoke_captured({"snowseek", "index", source.string(),
                                 "--index", destination.string()});
        snowseek::test::require_equal(
                partial.status, 2, "partial index command should return two");
        snowseek::test::require(
                partial.output.ends_with("warning_count=1\n") &&
                        partial.error.find("document warning:") !=
                                std::string::npos,
                "recoverable document diagnostics should use stderr and a "
                "stage label");
        snowseek::test::require_equal(
                invoke({"snowseek", "query", destination.string(), "one?"}), 1,
                "unsupported wildcard syntax should fail");
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

/** @brief Verifies update, Glob removal, compaction, and no-op output. */
void runs_incremental_maintenance_commands() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        const auto workspace_parent = temporary.path() / "workspace";
        std::filesystem::create_directory(source);
        std::filesystem::create_directory(workspace_parent);
        write_file(source / "a.txt", "alpha");
        snowseek::test::require_equal(
                invoke({"snowseek", "index", source.string(), "--index",
                        destination.string()}),
                0, "the maintenance fixture should build");

        write_file(source / "a.txt", "omega");
        write_file(source / "b.txt", "beta");
        const auto update = invoke_captured(
                {"snowseek", "update", source.string(), "--threads", "2",
                 "--index", destination.string(), "--profile", "minimal",
                 "--temporary-directory", workspace_parent.string()});
        require_output_keys(update, {"outcome", "revision", "segments", "added",
                                     "modified", "removed", "unchanged",
                                     "failed", "memory_peak_bytes",
                                     "temporary_peak_bytes", "warning_count"});
        snowseek::test::require(
                update.status == 0 &&
                        update.output.find("outcome=published\n") == 0 &&
                        update.output.find("segments=2\n") !=
                                std::string::npos &&
                        update.output.find("added=1") != std::string::npos &&
                        update.output.find("modified=1") != std::string::npos,
                "update should report its published delta and counters");
        const auto unchanged =
                invoke_captured({"snowseek", "update", source.string(),
                                 "--index", destination.string()});
        snowseek::test::require(
                unchanged.status == 0 &&
                        unchanged.output.find("outcome=unchanged\n") == 0 &&
                        unchanged.output.find("segments=2\n") !=
                                std::string::npos,
                "an idempotent update should report a no-op");

        const auto removed = invoke_captured(
                {"snowseek", "remove", destination.string(), "--path", "a.*",
                 "--path", "a.*", "--memory-limit", "1MiB",
                 "--temporary-directory", workspace_parent.string()});
        snowseek::test::require(
                removed.status == 0 &&
                        removed.output.find("outcome=published\n") == 0 &&
                        removed.output.find("matched=1\n") != std::string::npos,
                "remove should deduplicate patterns and report one match");
        require_output_keys(removed, {"outcome", "revision", "segments",
                                      "matched", "memory_peak_bytes",
                                      "temporary_peak_bytes", "warning_count"});
        const auto query =
                invoke_captured({"snowseek", "query", destination.string(),
                                 "omega", "--paths-only"});
        snowseek::test::require_equal(
                query.output, std::string{},
                "a Tombstone should hide the removed path from queries");

        const auto compact = invoke_captured(
                {"snowseek", "compact", destination.string(), "--threads", "3",
                 "--temporary-directory", workspace_parent.string()});
        snowseek::test::require(
                compact.status == 0 &&
                        compact.output.find("outcome=compacted\n") == 0 &&
                        compact.output.find("segments=1\n") !=
                                std::string::npos,
                "compact should accept shared resource options and publish");
        require_output_keys(compact, {"outcome", "revision", "segments",
                                      "discarded_records", "memory_peak_bytes",
                                      "temporary_peak_bytes", "warning_count"});
        const auto stats =
                invoke_captured({"snowseek", "stats", destination.string()});
        snowseek::test::require(
                stats.status == 0 &&
                        stats.output.find("documents=1\n") !=
                                std::string::npos &&
                        stats.output.find("segments=1\n") !=
                                std::string::npos &&
                        stats.output.find("tombstones=0\n") !=
                                std::string::npos,
                "stats should distinguish live, Segment, and Tombstone counts");

        snowseek::test::require_equal(
                invoke({"snowseek", "remove", destination.string()}), 1,
                "remove should require at least one --path");
        snowseek::test::require_equal(
                invoke({"snowseek", "compact", destination.string(), "--path",
                        "*.txt"}),
                1, "compact should reject remove-only path options");
        snowseek::test::require_equal(
                invoke({"snowseek", "update", source.string()}), 1,
                "update should require one --index directory");
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
                {"runs incremental maintenance commands",
                 runs_incremental_maintenance_commands},
        });
}
