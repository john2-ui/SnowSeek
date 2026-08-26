/**
 * @file persistent_query_test.cpp
 * @brief Exercises persistent indexing and public search semantics across
 * reopen.
 */

#include "snowseek/index.hpp"
#include "snowseek/search.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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
        std::filesystem::path path_; ///< Source and index fixture root.
};

/** @brief Writes a corpus text fixture. */
void write_file(const std::filesystem::path &path,
                const std::string &contents) {
        std::ofstream output(path, std::ios::binary);
        output << contents;
        if (!output) {
                throw std::runtime_error("failed to write persistent fixture");
        }
}

/** @brief Verifies build, reopen, ranking, explanation, and Top-K truncation.
 */
void builds_reopens_and_queries() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "Timeout retry timeout");
        write_file(source / "b.txt", "retry policy");

        const auto report = snowseek::IndexWriter(destination).rebuild(source);
        snowseek::test::require(report.diagnostics.empty(),
                                "valid corpus should persist completely");
        snowseek::test::require(
                std::filesystem::exists(destination /
                                        "segment-0000000000000001.idx"),
                "the stable Segment filename should be published");

        const snowseek::Searcher engine(destination);
        const auto term = engine.search("TIMEOUT");
        snowseek::test::require_equal(term.size(), std::size_t{1},
                                      "term query should survive reopen");
        snowseek::test::require_equal(term[0].path,
                                      std::filesystem::path("a.txt"),
                                      "query paths should be source-relative");
        snowseek::test::require(!term[0].snippet.has_value(),
                                "unrequested snippets should remain absent");
        const auto conjunction = engine.search("retry and policy");
        snowseek::test::require_equal(conjunction.size(), std::size_t{1},
                                      "case-insensitive AND should work");
        snowseek::SearchOptions options;
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
        static_cast<void>(snowseek::IndexWriter(destination).rebuild(source));
        const snowseek::Searcher engine(destination);

        const auto phrase = engine.search("\"alpha beta\"");
        snowseek::test::require_equal(
                phrase.size(), std::size_t{2},
                "phrase should require adjacent positions");
        snowseek::test::require_equal(
                engine.search("\"alpha alpha\"").size(), std::size_t{0},
                "nonadjacent repeated term should not match a phrase");
        snowseek::test::require_equal(
                engine.search("\"echo echo\"").size(), std::size_t{1},
                "adjacent repeated term should match a phrase");

        const auto boolean =
                engine.search("alpha AND (beta OR delta) AND NOT extension:md");
        snowseek::test::require_equal(
                boolean.size(), std::size_t{2},
                "Boolean expression should honor grouping");
        const auto path = engine.search("path:sub/*");
        snowseek::test::require_equal(path.size(), std::size_t{2},
                                      "path Glob should match relative paths");
        const auto extension = engine.search("extension:.mD");
        snowseek::test::require_equal(extension.size(), std::size_t{1},
                                      "extension filter should ignore case");
        snowseek::test::require_equal(
                extension[0].path, std::filesystem::path("b.md"),
                "extension filter should retain its path");
}

/** @brief Verifies positionless indexes reject only phrase evaluation. */
void rejects_phrases_without_positions() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "alpha beta alpha");
        snowseek::IndexOptions options;
        options.profile = snowseek::ResourceProfile::minimal;
        static_cast<void>(
                snowseek::IndexWriter(destination, options).rebuild(source));
        const snowseek::Searcher engine(destination);
        snowseek::test::require_equal(
                engine.search("alpha").size(), std::size_t{1},
                "positionless indexes should support term scoring");
        snowseek::test::require_throws<std::invalid_argument>(
                [&engine] {
                        static_cast<void>(engine.search("\"alpha beta\""));
                },
                "positionless indexes should reject phrase queries");
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
        static_cast<void>(snowseek::IndexWriter(destination).rebuild(source));
        const snowseek::Searcher engine(destination);

        snowseek::SearchOptions options;
        options.top_k = 2;
        options.explain = true;
        const auto ranked = engine.search("alpha", options);
        snowseek::test::require_equal(
                ranked[0].path, std::filesystem::path("a.txt"),
                "higher term frequency should rank first");
        snowseek::test::require(
                std::abs(ranked[0].score - ranked[0].explanation[0].score) <
                        0.0000001,
                "single-term explanation should sum to total score");

        const auto ties = engine.search("extension:txt");
        snowseek::test::require_equal(ties[0].path,
                                      std::filesystem::path("a.txt"),
                                      "zero-score ties should use path order");
        snowseek::test::require_equal(
                ties[1].path, std::filesystem::path("b.txt"),
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
        static_cast<void>(snowseek::IndexWriter(destination).rebuild(source));
        const snowseek::Searcher engine(destination);

        snowseek::SearchOptions options;
        options.source_root = source;
        options.top_k = 2;
        const auto results = engine.search("unique", options);
        const auto a = std::find_if(
                results.begin(), results.end(), [](const auto &result) {
                        return result.path == std::filesystem::path("a.txt");
                });
        snowseek::test::require(a != results.end(),
                                "snippet fixture should remain in Top-K");
        snowseek::test::require(a->snippet.has_value(),
                                "readable source should provide a snippet");
        snowseek::test::require_equal(a->snippet->line, std::size_t{2},
                                      "snippet should report one-based line");
        snowseek::test::require(a->snippet->text.find("Unicode 雪") !=
                                        std::string::npos,
                                "snippet should retain valid UTF-8");
        const auto truncated = engine.search("longtoken", options);
        snowseek::test::require(truncated[0].snippet.has_value() &&
                                        truncated[0].snippet->text.size() <=
                                                240,
                                "snippet should honor its byte limit");

        std::filesystem::remove(source / "a.txt");
        const auto stale = engine.search("term", options);
        snowseek::test::require(
                !stale[0].snippet.has_value(),
                "missing source should retain a hit without a snippet");
}

/** @brief Verifies invalid syntax, source roots, and limits fail. */
void rejects_invalid_queries_and_options() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "one two three");
        static_cast<void>(snowseek::IndexWriter(destination).rebuild(source));
        const snowseek::Searcher engine(destination);
        snowseek::test::require_throws<std::invalid_argument>(
                [&engine] { static_cast<void>(engine.search("one two")); },
                "implicit AND should be rejected");
        snowseek::SearchOptions excessive;
        excessive.top_k = snowseek::kMaxTopK + 1;
        snowseek::test::require_throws<std::invalid_argument>(
                [&engine, &excessive] {
                        static_cast<void>(engine.search("one", excessive));
                },
                "Top-K above the limit should fail");
        snowseek::SearchOptions missing_source;
        missing_source.source_root = temporary.path() / "missing";
        snowseek::test::require_throws<std::invalid_argument>(
                [&engine, &missing_source] {
                        static_cast<void>(engine.search("one", missing_source));
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

        const auto report = snowseek::IndexWriter(destination).rebuild(source);
        snowseek::test::require_equal(report.diagnostics.size(), std::size_t{1},
                                      "oversized tokens should be diagnosed");
        snowseek::test::require_equal(
                report.diagnostics[0].stage,
                snowseek::DiagnosticStage::document,
                "parse failures should retain their diagnostic stage");
        const snowseek::Searcher engine(destination);
        const auto matches = engine.search("safe");
        snowseek::test::require_equal(matches.size(), std::size_t{1},
                                      "successful documents should publish");
        snowseek::test::require_equal(
                matches[0].path, std::filesystem::path("a-valid.txt"),
                "partial index paths should remain stable");
}

/** @brief Verifies queries and BM25 use only visible cross-Segment records. */
void queries_incremental_segments_with_global_statistics() {
        const TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        const auto destination = temporary.path() / "index";
        std::filesystem::create_directory(source);
        write_file(source / "a.txt", "alpha beta shared");
        write_file(source / "b.txt", "stale shared");
        snowseek::IndexWriter writer(destination);
        static_cast<void>(writer.rebuild(source));
        const auto initial_stats = snowseek::validate_index(destination);
        snowseek::test::require(initial_stats.documents == 2 &&
                                        initial_stats.segments == 1 &&
                                        initial_stats.tombstones == 0,
                                "a full build should report one live Segment");

        write_file(source / "b.txt", "alpha beta replacement");
        write_file(source / "c.txt", "gamma shared");
        static_cast<void>(writer.update(source));
        const auto incremental_stats = snowseek::validate_index(destination);
        snowseek::test::require(
                incremental_stats.documents == 3 &&
                        incremental_stats.segments == 2 &&
                        incremental_stats.tombstones == 0,
                "an update should report logical documents across Segments");
        snowseek::SearchOptions explained;
        explained.explain = true;
        const snowseek::Searcher incremental(destination);
        const auto phrase = incremental.search("\"alpha beta\"");
        const auto shared = incremental.search("shared", explained);
        snowseek::test::require(
                phrase.size() == 2 && shared.size() == 2 &&
                        shared[0].explanation[0].document_frequency == 2 &&
                        shared[1].explanation[0].document_frequency == 2,
                "phrase and BM25 document frequency should span visible "
                "Segments");

        const std::vector<std::string> patterns{"a.txt"};
        static_cast<void>(writer.remove(patterns));
        const auto removed_stats = snowseek::validate_index(destination);
        snowseek::test::require(
                removed_stats.documents == 2 && removed_stats.segments == 3 &&
                        removed_stats.tombstones == 1,
                "removal statistics should separate live and physical data");
        const snowseek::Searcher after_remove(destination);
        const auto remaining_phrase = after_remove.search("\"alpha beta\"");
        const auto remaining_shared = after_remove.search("shared", explained);
        snowseek::test::require(
                remaining_phrase.size() == 1 &&
                        remaining_phrase[0].path ==
                                std::filesystem::path("b.txt") &&
                        remaining_shared.size() == 1 &&
                        remaining_shared[0].path ==
                                std::filesystem::path("c.txt") &&
                        remaining_shared[0].explanation[0].document_frequency ==
                                1,
                "overridden and Tombstone records must not affect queries");

        const auto score_before = remaining_shared[0].score;
        static_cast<void>(writer.compact());
        const auto compacted_stats = snowseek::validate_index(destination);
        snowseek::test::require(compacted_stats.documents == 2 &&
                                        compacted_stats.segments == 1 &&
                                        compacted_stats.tombstones == 0,
                                "compaction should remove physical Tombstones");
        const snowseek::Searcher after_compact(destination);
        const auto compacted = after_compact.search("shared", explained);
        snowseek::test::require(
                compacted.size() == 1 &&
                        std::abs(compacted[0].score - score_before) < 0.0000001,
                "compaction should preserve BM25 results and explanation "
                "inputs");
}

} // namespace

/** @brief Runs persistent build-and-query integration tests. */
int main() {
        return snowseek::test::run({
                {"builds, reopens, and queries", builds_reopens_and_queries},
                {"evaluates M3 query language", evaluates_m3_query_language},
                {"rejects phrases without positions",
                 rejects_phrases_without_positions},
                {"ranks deterministically", ranks_deterministically},
                {"loads Top-K snippets", loads_top_k_snippets},
                {"rejects invalid queries and options",
                 rejects_invalid_queries_and_options},
                {"publishes partial index with diagnostics",
                 publishes_partial_index_with_diagnostics},
                {"queries incremental Segments with global statistics",
                 queries_incremental_segments_with_global_statistics},
        });
}
