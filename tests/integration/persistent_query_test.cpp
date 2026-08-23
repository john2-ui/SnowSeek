#include "snowseek/index/index_builder.hpp"
#include "snowseek/query/query_engine.hpp"
#include "snowseek/storage/index_file.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

class TemporaryDirectory {
      public:
        /** @brief Creates source and index directories for one test. */
        TemporaryDirectory() {
                const auto seed = std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count();
                path_ = std::filesystem::temp_directory_path() /
                        ("snowseek-persistent-query-test-" +
                         std::to_string(seed));
                std::filesystem::create_directory(path_);
        }

        /** @brief Removes the test directories recursively. */
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

/** @brief Writes a corpus text fixture. */
void write_file(const std::filesystem::path &path, const std::string &contents) {
        std::ofstream output(path, std::ios::binary);
        output << contents;
        if (!output) {
                throw std::runtime_error("failed to write persistent fixture");
        }
}

/** @brief Verifies build, reopen, ranking, explanation, and Top-K truncation. */
void builds_reopens_and_queries() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "Timeout retry timeout");
        write_file(source / "b.txt", "retry policy");

        const auto report =
                snowseek::index::IndexBuilder{}.build(source, destination);
        snowseek::test::require(report.scan_errors.empty() &&
                                        report.document_errors.empty(),
                                "valid corpus should persist completely");
        snowseek::test::require(
                std::filesystem::exists(destination /
                                        snowseek::storage::kSegmentFileName),
                "the stable Segment filename should be published");

        const snowseek::query::QueryEngine engine(destination);
        const auto term = engine.search("TIMEOUT");
        snowseek::test::require_equal(term.size(), std::size_t{1},
                                      "term query should survive reopen");
        snowseek::test::require_equal(term[0].path,
                                      std::filesystem::path("a.txt"),
                                      "query paths should be source-relative");
        const auto conjunction = engine.search("retry and policy");
        snowseek::test::require_equal(conjunction.size(), std::size_t{1},
                                      "case-insensitive AND should work");
        snowseek::query::SearchOptions options;
        options.top_k = 1;
        options.explain = true;
        const auto top = engine.search("retry", options);
        snowseek::test::require_equal(top.size(), std::size_t{1},
                                      "Top-K should retain one result");
        snowseek::test::require(!top[0].explanation.empty(),
                                "explanation should contain term scores");
}

/** @brief Verifies Boolean, phrase, and path-filter semantics. */
void evaluates_m3_query_language() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        std::filesystem::create_directory(source / "sub");
        write_file(source / "a.txt", "alpha beta alpha");
        write_file(source / "b.md", "alpha gamma beta");
        write_file(source / "sub" / "c.TXT", "delta alpha beta");
        write_file(source / "sub" / "repeat.txt", "echo echo");
        static_cast<void>(
                snowseek::index::IndexBuilder{}.build(source, destination));
        const snowseek::query::QueryEngine engine(destination);

        const auto phrase = engine.search("\"alpha beta\"");
        snowseek::test::require_equal(phrase.size(), std::size_t{2},
                                      "phrase should require adjacent positions");
        snowseek::test::require_equal(
                engine.search("\"alpha alpha\"").size(), std::size_t{0},
                "nonadjacent repeated term should not match a phrase");
        snowseek::test::require_equal(
                engine.search("\"echo echo\"").size(), std::size_t{1},
                "adjacent repeated term should match a phrase");

        const auto boolean = engine.search(
                "alpha AND (beta OR delta) AND NOT extension:md");
        snowseek::test::require_equal(boolean.size(), std::size_t{2},
                                      "Boolean expression should honor grouping");
        const auto path = engine.search("path:sub/*");
        snowseek::test::require_equal(path.size(), std::size_t{2},
                                      "path Glob should match relative paths");
        const auto extension = engine.search("extension:.mD");
        snowseek::test::require_equal(extension.size(), std::size_t{1},
                                      "extension filter should ignore case");
        snowseek::test::require_equal(extension[0].path,
                                      std::filesystem::path("b.md"),
                                      "extension filter should retain its path");
}

/** @brief Verifies ranking is stable and score explanations sum exactly. */
void ranks_deterministically() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "alpha alpha alpha");
        write_file(source / "b.txt", "alpha");
        write_file(source / "c.txt", "filter only");
        static_cast<void>(
                snowseek::index::IndexBuilder{}.build(source, destination));
        const snowseek::query::QueryEngine engine(destination);

        snowseek::query::SearchOptions options;
        options.top_k = 2;
        options.explain = true;
        const auto ranked = engine.search("alpha", options);
        snowseek::test::require_equal(ranked[0].path,
                                      std::filesystem::path("a.txt"),
                                      "higher term frequency should rank first");
        snowseek::test::require(
                std::abs(ranked[0].score - ranked[0].explanation[0].score) <
                        0.0000001,
                "single-term explanation should sum to total score");

        const auto ties = engine.search("extension:txt");
        snowseek::test::require_equal(ties[0].path,
                                      std::filesystem::path("a.txt"),
                                      "zero-score ties should use path order");
        snowseek::test::require_equal(ties[1].path,
                                      std::filesystem::path("b.txt"),
                                      "tie order should not depend on traversal");
}

/** @brief Verifies Top-K snippets and stale source handling. */
void loads_top_k_snippets() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "heading\nUnicode 雪 unique term\n");
        write_file(source / "b.txt", "unique\n");
        write_file(source / "long.txt",
                   "longtoken " + std::string(300, '.') + "\n");
        static_cast<void>(
                snowseek::index::IndexBuilder{}.build(source, destination));
        const snowseek::query::QueryEngine engine(destination);

        snowseek::query::SearchOptions options;
        options.source_root = source;
        options.top_k = 2;
        const auto results = engine.search("unique", options);
        const auto a = std::find_if(
                results.begin(), results.end(), [](const auto &result) {
                        return result.path == std::filesystem::path("a.txt");
                });
        snowseek::test::require(a != results.end(),
                                "snippet fixture should remain in Top-K");
        snowseek::test::require_equal(a->line, std::size_t{2},
                                      "snippet should report one-based line");
        snowseek::test::require(a->snippet.find("Unicode 雪") !=
                                        std::string::npos,
                                "snippet should retain valid UTF-8");
        const auto truncated = engine.search("longtoken", options);
        snowseek::test::require(truncated[0].snippet.size() <= 240,
                                "snippet should honor its byte limit");

        std::filesystem::remove(source / "a.txt");
        const auto stale = engine.search("term", options);
        snowseek::test::require_equal(stale[0].line, std::size_t{0},
                                      "missing source should retain result");
        snowseek::test::require(stale[0].snippet.empty(),
                                "missing source should leave snippet empty");
}

/** @brief Verifies invalid syntax, source roots, and limits fail. */
void rejects_invalid_queries_and_options() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "one two three");
        static_cast<void>(
                snowseek::index::IndexBuilder{}.build(source, destination));
        const snowseek::query::QueryEngine engine(destination);
        snowseek::test::require_throws<std::invalid_argument>(
                [&engine] {
                        static_cast<void>(engine.search("one two"));
                },
                "implicit AND should be rejected");
        snowseek::query::SearchOptions excessive;
        excessive.top_k = snowseek::query::kMaxTopK + 1;
        snowseek::test::require_throws<std::invalid_argument>(
                [&engine, &excessive] {
                        static_cast<void>(engine.search("one", excessive));
                },
                "Top-K above the limit should fail");
        snowseek::query::SearchOptions missing_source;
        missing_source.source_root = temporary.path() / "missing";
        snowseek::test::require_throws<std::invalid_argument>(
                [&engine, &missing_source] {
                        static_cast<void>(
                                engine.search("one", missing_source));
                },
                "invalid source root should fail");
}

/** @brief Verifies successful documents are published after a file failure. */
void publishes_partial_index_with_diagnostics() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a-valid.txt", "safe");
        write_file(source / "b-invalid.txt", std::string(300, 'x'));

        const auto report =
                snowseek::index::IndexBuilder{}.build(source, destination);
        snowseek::test::require_equal(report.document_errors.size(),
                                      std::size_t{1},
                                      "oversized tokens should be diagnosed");
        const snowseek::query::QueryEngine engine(destination);
        const auto matches = engine.search("safe");
        snowseek::test::require_equal(matches.size(), std::size_t{1},
                                      "successful documents should publish");
        snowseek::test::require_equal(matches[0].path,
                                      std::filesystem::path("a-valid.txt"),
                                      "partial index paths should remain stable");
}

} // namespace

/** @brief Runs persistent build-and-query integration tests. */
int main() {
        return snowseek::test::run({
                {"builds, reopens, and queries", builds_reopens_and_queries},
                {"evaluates M3 query language", evaluates_m3_query_language},
                {"ranks deterministically", ranks_deterministically},
                {"loads Top-K snippets", loads_top_k_snippets},
                {"rejects invalid queries and options",
                 rejects_invalid_queries_and_options},
                {"publishes partial index with diagnostics",
                 publishes_partial_index_with_diagnostics},
        });
}
