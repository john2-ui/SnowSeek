/**
 * @file text_pipeline_test.cpp
 * @brief Verifies streamed text reading and tokenization as an integrated
 * pipeline.
 */

#include "analysis/tokenizer.hpp"
#include "document/text_reader.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
                        path_ = base / ("snowseek-pipeline-test-" +
                                        std::to_string(seed) + "-" +
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
        std::filesystem::path path_; ///< Pipeline fixture root.
};

/** @brief Verifies equivalence between streamed and one-shot tokenization. */
void produces_the_same_tokens_for_streamed_input() {
        const TemporaryDirectory temporary;
        const auto path = temporary.path() / "input.txt";
        const std::string contents =
                "Alpha retry_policy camelCase HTTP2 finalToken";
        std::ofstream output(path, std::ios::binary);
        output << contents;
        output.close();
        snowseek::test::require(static_cast<bool>(output),
                                "the integration test input should be written");

        const snowseek::analysis::Tokenizer tokenizer;
        const auto expected = tokenizer.tokenize_with_positions(contents);

        snowseek::document::TextReadOptions read_options;
        read_options.chunk_size = 1;
        const snowseek::document::TextReader reader(read_options);
        snowseek::analysis::TokenizerSession session;
        std::vector<snowseek::analysis::Token> actual;
        const snowseek::analysis::TokenConsumer consumer =
                [&actual](snowseek::analysis::Token token) {
                        actual.push_back(std::move(token));
                };

        const auto stats = reader.read(
                path, [&session, &consumer](std::string_view chunk) {
                        session.push(chunk, consumer);
                });
        session.finish(consumer);

        snowseek::test::require_equal(
                stats.bytes_read, static_cast<std::uint64_t>(contents.size()),
                "the pipeline should read the complete source file");
        snowseek::test::require_equal(
                actual.size(), expected.size(),
                "streamed and one-shot token counts should match");
        for (std::size_t index = 0; index < expected.size(); ++index) {
                snowseek::test::require_equal(
                        actual[index].term, expected[index].term,
                        "streamed token text should match one-shot output");
                snowseek::test::require_equal(
                        actual[index].position, expected[index].position,
                        "streamed positions should match one-shot output");
        }
}

} // namespace

/** @brief Runs the reader-to-tokenizer integration test. */
int main() {
        return snowseek::test::run({
                {"matches one-shot tokenization for byte-sized chunks",
                 produces_the_same_tokens_for_streamed_input},
        });
}
