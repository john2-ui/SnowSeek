#include "snowseek/index/index_builder.hpp"
#include "snowseek/query/query_engine.hpp"
#include "snowseek/storage/index_file.hpp"

#include "test_support.hpp"

#include <chrono>
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

/** @brief Verifies build, reopen, term query, AND query, and Top-K truncation. */
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
        snowseek::test::require_equal(engine.search("retry", 1).size(),
                                      std::size_t{1},
                                      "Top-K should truncate ordered results");
}

/** @brief Verifies invalid M2 expressions fail after reopening. */
void rejects_unsupported_query_grammar() {
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
                        static_cast<void>(engine.search("one OR two"));
                },
                "unsupported operators should be rejected");
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
                {"rejects unsupported query grammar",
                 rejects_unsupported_query_grammar},
                {"publishes partial index with diagnostics",
                 publishes_partial_index_with_diagnostics},
        });
}
