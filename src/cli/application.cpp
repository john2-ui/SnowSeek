#include "snowseek/cli/application.hpp"

#include "snowseek/common/version.hpp"
#include "snowseek/index/index_builder.hpp"
#include "snowseek/query/query_engine.hpp"
#include "snowseek/storage/index_file.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace snowseek::cli {
namespace {

/** @brief Prints command-line usage and the current program version. */
void print_help() {
        std::cout << "SnowSeek " << kVersion << "\n\n"
                  << "Usage:\n"
                  << "  snowseek index <source> --index <dir>\n"
                  << "  snowseek query <index> <expression>\n"
                  << "  snowseek stats|verify <index>\n";
}

/**
 * @brief Builds and publishes one persistent v1 Segment.
 * @param source Corpus root to scan.
 * @param index_directory Destination index directory.
 * @return Zero for a complete index or two when recoverable files were skipped.
 */
int run_index(const std::filesystem::path &source,
              const std::filesystem::path &index_directory) {
        const auto result =
                index::IndexBuilder{}.build(source, index_directory);
        for (const auto &error : result.scan_errors) {
                std::cerr << "scan warning: " << error.path << ": "
                          << error.error.message() << '\n';
        }
        for (const auto &error : result.document_errors) {
                std::cerr << "document warning: " << error.path << ": "
                          << error.message << '\n';
        }
        std::cout << "index=" << result.index_file.string() << '\n'
                  << "scanned=" << result.stats.scanned_files << '\n'
                  << "indexed=" << result.stats.indexed_files << '\n'
                  << "failed=" << result.stats.failed_files << '\n'
                  << "tokens=" << result.stats.token_count << '\n';
        return result.scan_errors.empty() && result.document_errors.empty() ? 0
                                                                            : 2;
}

/**
 * @brief Executes an M2 query and prints one relative path per match.
 * @param index_directory Directory containing the v1 Segment.
 * @param expression One term or a strict two-term AND expression.
 * @return Zero after a successful query, including an empty result.
 */
int run_query(const std::filesystem::path &index_directory,
              std::string_view expression) {
        const query::QueryEngine engine(index_directory);
        for (const auto &path : engine.search(expression)) {
                std::cout << path.string() << '\n';
        }
        return 0;
}

/**
 * @brief Prints stable key-value statistics for a validated index.
 * @param index_directory Directory containing the v1 Segment.
 * @return Zero after successful validation and output.
 */
int run_stats(const std::filesystem::path &index_directory) {
        const auto stats = storage::read_index_file(
                                   index_directory / storage::kSegmentFileName)
                                   .stats;
        std::cout << "documents=" << stats.document_count << '\n'
                  << "terms=" << stats.term_count << '\n'
                  << "postings=" << stats.posting_count << '\n'
                  << "positions=" << stats.position_count << '\n'
                  << "bytes=" << stats.file_size << '\n';
        return 0;
}

/**
 * @brief Fully validates an index and reports a concise success message.
 * @param index_directory Directory containing the v1 Segment.
 * @return Zero when every structural and checksum invariant holds.
 */
int run_verify(const std::filesystem::path &index_directory) {
        static_cast<void>(storage::read_index_file(
                index_directory / storage::kSegmentFileName));
        std::cout << "index verified\n";
        return 0;
}

} // namespace

int run(int argc, char *argv[]) {
        try {
                if (argc < 2 || std::string_view(argv[1]) == "--help" ||
                    std::string_view(argv[1]) == "-h") {
                        print_help();
                        return 0;
                }
                if (std::string_view(argv[1]) == "--version") {
                        std::cout << kVersion << '\n';
                        return 0;
                }

                // Dispatch only the frozen M2 command shapes so malformed
                // invocations fail before touching an index or corpus.
                const std::string_view command(argv[1]);
                if (command == "index" && argc == 5 &&
                    std::string_view(argv[3]) == "--index") {
                        return run_index(argv[2], argv[4]);
                }
                if (command == "query" && argc == 4) {
                        return run_query(argv[2], argv[3]);
                }
                if (command == "stats" && argc == 3) {
                        return run_stats(argv[2]);
                }
                if (command == "verify" && argc == 3) {
                        return run_verify(argv[2]);
                }
                throw std::invalid_argument("invalid command arguments");
        } catch (const std::exception &error) {
                std::cerr << "snowseek: " << error.what() << '\n';
                return 1;
        }
}

} // namespace snowseek::cli
