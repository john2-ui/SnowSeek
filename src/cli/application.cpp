#include "snowseek/cli/application.hpp"

#include "snowseek/common/version.hpp"
#include "snowseek/index/index_builder.hpp"
#include "snowseek/query/query_engine.hpp"
#include "snowseek/storage/index_file.hpp"

#include <charconv>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace snowseek::cli {
namespace {

enum class QueryOutputFormat {
        rich_text,
        jsonl,
        paths_only,
};

struct QueryCommandOptions {
        query::SearchOptions search;
        QueryOutputFormat output = QueryOutputFormat::rich_text;
};

/** @brief Prints command-line usage and the current program version. */
void print_help() {
        std::cout << "SnowSeek " << kVersion << "\n\n"
                  << "Usage:\n"
                  << "  snowseek index <source> --index <dir>\n"
                  << "  snowseek query <index> <expression> [options]\n"
                  << "  snowseek stats|verify <index>\n";
        std::cout << "\nQuery options:\n"
                  << "  --source <dir>   Read Top-K source snippets\n"
                  << "  --top-k <N>      Return at most N results (max 1000)\n"
                  << "  --jsonl          Emit one JSON object per result\n"
                  << "  --paths-only     Emit one relative path per result\n"
                  << "  --explain        Include per-term BM25 contributions\n";
}

/**
 * @brief Parses a decimal Top-K argument without accepting trailing bytes.
 * @param text Command-line value.
 * @return Parsed nonnegative result limit.
 * @throws std::invalid_argument If text is not a canonical decimal integer.
 */
[[nodiscard]] std::size_t parse_top_k(std::string_view text) {
        std::size_t value = 0;
        const auto result =
                std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || result.ec != std::errc{} ||
            result.ptr != text.data() + text.size()) {
                throw std::invalid_argument("--top-k requires an integer");
        }
        return value;
}

/**
 * @brief Selects one mutually exclusive query output mode.
 * @param options Parsed command options to update.
 * @param format Newly requested mode.
 * @throws std::invalid_argument If another nondefault mode was already chosen.
 */
void select_output(QueryCommandOptions &options, QueryOutputFormat format) {
        if (options.output != QueryOutputFormat::rich_text) {
                throw std::invalid_argument(
                        "--jsonl and --paths-only are mutually exclusive");
        }
        options.output = format;
}

/**
 * @brief Parses optional arguments following a query expression.
 * @param argc Full process argument count.
 * @param argv Writable process argument vector.
 * @return Validated presentation and search options.
 * @throws std::invalid_argument If an option is unknown, incomplete, or
 * conflicts with another option.
 */
[[nodiscard]] QueryCommandOptions parse_query_options(int argc, char *argv[]) {
        QueryCommandOptions options;
        bool has_source = false;
        bool has_top_k = false;
        for (int index = 4; index < argc; ++index) {
                const std::string_view option(argv[index]);
                if (option == "--source") {
                        if (has_source || index + 1 >= argc) {
                                throw std::invalid_argument(
                                        "--source requires one unique directory");
                        }
                        options.search.source_root = argv[++index];
                        has_source = true;
                } else if (option == "--top-k") {
                        if (has_top_k || index + 1 >= argc) {
                                throw std::invalid_argument(
                                        "--top-k requires one unique value");
                        }
                        options.search.top_k = parse_top_k(argv[++index]);
                        has_top_k = true;
                } else if (option == "--jsonl") {
                        select_output(options, QueryOutputFormat::jsonl);
                } else if (option == "--paths-only") {
                        select_output(options, QueryOutputFormat::paths_only);
                } else if (option == "--explain") {
                        options.search.explain = true;
                } else {
                        throw std::invalid_argument("unknown query option: " +
                                                    std::string(option));
                }
        }
        if (options.output == QueryOutputFormat::paths_only &&
            options.search.explain) {
                throw std::invalid_argument(
                        "--paths-only cannot be combined with --explain");
        }
        return options;
}

/**
 * @brief Formats a score with stable locale-independent precision.
 * @param score BM25 value to format.
 * @return Fixed-point decimal with six fractional digits.
 */
[[nodiscard]] std::string format_score(double score) {
        std::ostringstream output;
        output << std::fixed << std::setprecision(6) << score;
        return output.str();
}

/**
 * @brief Escapes arbitrary UTF-8 bytes for a JSON string literal body.
 * @param text Unescaped bytes.
 * @return JSON-safe contents without surrounding quotation marks.
 */
[[nodiscard]] std::string json_escape(std::string_view text) {
        constexpr char digits[] = "0123456789abcdef";
        std::string escaped;
        escaped.reserve(text.size());
        for (const unsigned char character : text) {
                switch (character) {
                case '"':
                        escaped.append("\\\"");
                        break;
                case '\\':
                        escaped.append("\\\\");
                        break;
                case '\b':
                        escaped.append("\\b");
                        break;
                case '\f':
                        escaped.append("\\f");
                        break;
                case '\n':
                        escaped.append("\\n");
                        break;
                case '\r':
                        escaped.append("\\r");
                        break;
                case '\t':
                        escaped.append("\\t");
                        break;
                default:
                        if (character < 0x20U) {
                                escaped.append("\\u00");
                                escaped.push_back(digits[character >> 4U]);
                                escaped.push_back(digits[character & 0x0fU]);
                        } else {
                                escaped.push_back(static_cast<char>(character));
                        }
                }
        }
        return escaped;
}

/**
 * @brief Prints one human-readable ranked result and optional explanation.
 * @param result Search result to render.
 */
void print_rich_result(const query::SearchResult &result) {
        std::cout << result.path.generic_string();
        if (result.line != 0) {
                std::cout << ':' << result.line;
        }
        std::cout << " score=" << format_score(result.score);
        if (!result.snippet.empty()) {
                std::cout << ' ' << result.snippet;
        }
        std::cout << '\n';
        for (const auto &contribution : result.explanation) {
                std::cout << "  " << contribution.term
                          << " tf=" << contribution.term_frequency
                          << " df=" << contribution.document_frequency
                          << " score=" << format_score(contribution.score)
                          << '\n';
        }
}

/**
 * @brief Prints one result using the stable JSONL field schema.
 * @param result Search result to render.
 * @param include_explanation Whether to include the explanation array.
 */
void print_json_result(const query::SearchResult &result,
                       bool include_explanation) {
        std::cout << "{\"path\":\""
                  << json_escape(result.path.generic_string())
                  << "\",\"line\":" << result.line << ",\"score\":"
                  << format_score(result.score) << ",\"snippet\":\""
                  << json_escape(result.snippet) << '"';
        if (include_explanation) {
                std::cout << ",\"explanation\":[";
                for (std::size_t index = 0;
                     index < result.explanation.size(); ++index) {
                        if (index != 0) {
                                std::cout << ',';
                        }
                        const auto &contribution = result.explanation[index];
                        std::cout << "{\"term\":\""
                                  << json_escape(contribution.term)
                                  << "\",\"tf\":"
                                  << contribution.term_frequency
                                  << ",\"df\":"
                                  << contribution.document_frequency
                                  << ",\"score\":"
                                  << format_score(contribution.score) << '}';
                }
                std::cout << ']';
        }
        std::cout << "}\n";
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
                  << "tokens=" << result.stats.token_count << '\n'
                  << "memory_metadata_bytes="
                  << result.stats.memory.metadata_bytes << '\n'
                  << "memory_reader_peak_bytes="
                  << result.stats.memory.reader_peak_bytes << '\n'
                  << "memory_token_peak_bytes="
                  << result.stats.memory.token_peak_bytes << '\n'
                  << "memory_dictionary_bytes="
                  << result.stats.memory.dictionary_bytes << '\n'
                  << "memory_posting_bytes="
                  << result.stats.memory.posting_bytes << '\n'
                  << "memory_estimated_peak_bytes="
                  << result.stats.memory.estimated_peak_bytes << '\n';
        return result.scan_errors.empty() && result.document_errors.empty() ? 0
                                                                            : 2;
}

/**
 * @brief Executes an M3 query and renders ranked results.
 * @param index_directory Directory containing the v1 Segment.
 * @param expression Complete Boolean query expression.
 * @param options Search and presentation options.
 * @return Zero after a successful query, including an empty result.
 */
int run_query(const std::filesystem::path &index_directory,
              std::string_view expression,
              const QueryCommandOptions &options) {
        const query::QueryEngine engine(index_directory);
        for (const auto &result : engine.search(expression, options.search)) {
                switch (options.output) {
                case QueryOutputFormat::rich_text:
                        print_rich_result(result);
                        break;
                case QueryOutputFormat::jsonl:
                        print_json_result(result, options.search.explain);
                        break;
                case QueryOutputFormat::paths_only:
                        std::cout << result.path.generic_string() << '\n';
                        break;
                }
        }
        return 0;
}

/**
 * @brief Prints stable key-value statistics for a validated index.
 * @param index_directory Directory containing the v1 Segment.
 * @return Zero after successful validation and output.
 */
int run_stats(const std::filesystem::path &index_directory) {
        const auto stats = storage::validate_index_file(
                index_directory / storage::kSegmentFileName);
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
        static_cast<void>(storage::validate_index_file(
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

                // Dispatch only documented command shapes so malformed
                // invocations fail before touching an index or corpus.
                const std::string_view command(argv[1]);
                if (command == "index" && argc == 5 &&
                    std::string_view(argv[3]) == "--index") {
                        return run_index(argv[2], argv[4]);
                }
                if (command == "query" && argc >= 4) {
                        return run_query(argv[2], argv[3],
                                         parse_query_options(argc, argv));
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
