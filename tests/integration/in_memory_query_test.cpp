#include "snowseek/index/index_builder.hpp"
#include "snowseek/query/query_engine.hpp"

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
        /** @brief Creates a unique temporary corpus directory. */
        TemporaryDirectory() {
                const auto seed = std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count();
                const auto base = std::filesystem::temp_directory_path();
                for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
                        path_ = base /
                                ("snowseek-query-test-" + std::to_string(seed) +
                                 "-" + std::to_string(attempt));
                        std::error_code error;
                        if (std::filesystem::create_directory(path_, error)) {
                                return;
                        }
                }
                throw std::runtime_error(
                        "failed to create a temporary query-test directory");
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;
        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        /** @brief Removes the temporary corpus directory recursively. */
        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        /** @brief Returns the temporary corpus directory path. */
        [[nodiscard]] const std::filesystem::path &path() const {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

/**
 * @brief Writes one query corpus fixture.
 * @param path Destination file path.
 * @param contents Text bytes to index.
 * @throws std::runtime_error If the fixture cannot be written.
 */
void write_file(const std::filesystem::path &path, std::string_view contents) {
        std::ofstream output(path, std::ios::binary);
        output.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        if (!output) {
                throw std::runtime_error("failed to write query-test file");
        }
}

struct QueryFixture {
        TemporaryDirectory temporary;
        snowseek::index::InMemoryBuildResult build_result;

        /** @brief Builds a fixed three-document corpus for query tests. */
        QueryFixture() {
                write_file(temporary.path() / "a.txt", "Timeout retry timeout");
                write_file(temporary.path() / "b.txt", "retry policy");
                write_file(temporary.path() / "c.txt", "policy");
                build_result = snowseek::index::InMemoryIndexBuilder{}.build(
                        temporary.path());
                snowseek::test::require(
                        build_result.scan_errors.empty() &&
                                build_result.document_errors.empty(),
                        "the query fixture should build without errors");
        }
};

/** @brief Verifies normalized term lookup, frequencies, and positions. */
void searches_one_normalized_term() {
        const QueryFixture fixture;
        const snowseek::query::InMemoryQueryEngine engine(
                fixture.build_result.documents, fixture.build_result.index);
        const auto matches = engine.search_term("TIMEOUT");

        snowseek::test::require_equal(matches.size(), std::size_t{1},
                                      "timeout should match one document");
        snowseek::test::require_equal(
                matches[0].document_id, snowseek::document::DocumentId{0},
                "the term match should retain its document id");
        snowseek::test::require_equal(
                matches[0].path, fixture.temporary.path() / "a.txt",
                "the term match should map to its source path");
        snowseek::test::require_equal(matches[0].term_frequency,
                                      std::uint32_t{2},
                                      "the term match should expose frequency");
        snowseek::test::require_equal(
                matches[0].positions,
                std::vector<snowseek::index::Position>{0, 2},
                "the term match should copy both positions");
}

/** @brief Verifies missing terms return an empty result. */
void returns_empty_for_a_missing_term() {
        const QueryFixture fixture;
        const snowseek::query::InMemoryQueryEngine engine(
                fixture.build_result.documents, fixture.build_result.index);
        snowseek::test::require(engine.search_term("missing").empty(),
                                "a missing term should have no matches");
}

/** @brief Verifies enforcement of the one-token M1 query grammar. */
void rejects_invalid_term_inputs() {
        const QueryFixture fixture;
        const snowseek::query::InMemoryQueryEngine engine(
                fixture.build_result.documents, fixture.build_result.index);
        snowseek::test::require_throws<std::invalid_argument>(
                [&engine] { static_cast<void>(engine.search_term("")); },
                "empty query input should be rejected");
        snowseek::test::require_throws<std::invalid_argument>(
                [&engine] {
                        static_cast<void>(engine.search_term("retry policy"));
                },
                "multi-token query input should be rejected");
}

/** @brief Verifies two-term AND results and path mapping. */
void searches_two_terms_with_and() {
        const QueryFixture fixture;
        const snowseek::query::InMemoryQueryEngine engine(
                fixture.build_result.documents, fixture.build_result.index);
        const auto matches = engine.search_and("RETRY", "policy");

        snowseek::test::require_equal(matches.size(), std::size_t{1},
                                      "retry AND policy should match once");
        snowseek::test::require_equal(
                matches[0].document_id, snowseek::document::DocumentId{1},
                "AND should retain the common document id");
        snowseek::test::require_equal(
                matches[0].path, fixture.temporary.path() / "b.txt",
                "AND should map the common document path");
}

/** @brief Verifies missing and identical terms in AND queries. */
void handles_missing_and_identical_terms() {
        const QueryFixture fixture;
        const snowseek::query::InMemoryQueryEngine engine(
                fixture.build_result.documents, fixture.build_result.index);
        snowseek::test::require(
                engine.search_and("retry", "missing").empty(),
                "a missing AND operand should yield no matches");

        const auto identical = engine.search_and("retry", "retry");
        snowseek::test::require_equal(
                identical.size(), std::size_t{2},
                "identical terms should return every containing document");
        snowseek::test::require_equal(
                identical[0].document_id, snowseek::document::DocumentId{0},
                "identical-term results should remain ordered");
        snowseek::test::require_equal(
                identical[1].document_id, snowseek::document::DocumentId{1},
                "identical-term results should remain ordered");
}

} // namespace

/** @brief Runs the in-memory query integration-test suite. */
int main() {
        return snowseek::test::run({
                {"searches one normalized term", searches_one_normalized_term},
                {"returns empty for a missing term",
                 returns_empty_for_a_missing_term},
                {"rejects invalid term inputs", rejects_invalid_term_inputs},
                {"searches two terms with AND", searches_two_terms_with_and},
                {"handles missing and identical terms",
                 handles_missing_and_identical_terms},
        });
}
