/**
 * @file application.cpp
 * @brief Parses SnowSeek commands and renders command-line results.
 */

#include "cli/application.hpp"

#include "snowseek/index.hpp"
#include "snowseek/search.hpp"
#include "snowseek/version.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace snowseek::cli {
namespace {

enum class QueryOutputFormat {
        rich_text,
        jsonl,
        paths_only,
};

enum class MaintenanceCommand {
        rebuild,
        update,
        remove,
        compact,
};

struct SourceCommandOptions {
        std::filesystem::path index_directory; ///< Destination index path.
        IndexOptions index; ///< Resource settings for the maintenance command.
};

struct RemoveCommandOptions {
        IndexOptions index; ///< Resource settings for deletion publication.
        std::vector<std::string> path_patterns; ///< POSIX Globs to Tombstone.
};

struct QueryCommandOptions {
        SearchOptions search; ///< Result and presentation-data settings.
        QueryOutputFormat output = QueryOutputFormat::rich_text; ///< Selected output encoding.
};

/** @brief Prints command-line usage and the current program version. */
void print_help() {
        std::cout << "SnowSeek " << kVersion << "\n\n"
                  << "Usage:\n"
                  << "  snowseek index <source> --index <dir> [options]\n"
                  << "  snowseek update <source> --index <dir> [options]\n"
                  << "  snowseek remove <index> --path <glob> [options]\n"
                  << "  snowseek compact <index> [options]\n"
                  << "  snowseek query <index> <expression> [options]\n"
                  << "  snowseek stats|verify <index>\n";
        std::cout << "\nIndex options:\n"
                  << "  --temporary-directory <dir>   Place build workspace "
                     "in existing dir\n"
                  << "  --temporary-space-limit <size>  Limit private build "
                     "bytes\n"
                  << "  --memory-limit <size>           Limit classified build "
                     "memory\n"
                  << "  --threads <N>                  Parse at most N files "
                     "concurrently\n"
                  << "  --profile <name>               minimal, balanced, or "
                     "performance\n"
                  << "  --merge-fan-in <N>              Merge at most N "
                     "Segments (min 2)\n"
                  << "  Sizes accept B, KiB, MiB, GiB, or TiB suffixes\n"
                  << "  remove accepts repeated --path options\n"
                  << "\nQuery options:\n"
                  << "  --source <dir>   Read Top-K source snippets\n"
                  << "  --top-k <N>      Return at most N results (max 1000)\n"
                  << "  --jsonl          Emit one JSON object per result\n"
                  << "  --paths-only     Emit one relative path per result\n"
                  << "  --explain        Include per-term BM25 contributions\n";
}

/**
 * @brief Parses a positive resource size with an optional IEC suffix.
 * @param text Decimal integer followed by B, KiB, MiB, GiB, TiB, or nothing.
 * @param option_name CLI option named in diagnostics.
 * @return Size converted to bytes.
 * @throws std::invalid_argument If the number or suffix is malformed or zero.
 * @throws std::overflow_error If conversion exceeds std::uint64_t.
 */
[[nodiscard]] std::uint64_t
parse_byte_size(std::string_view text,
                std::string_view option_name = "--temporary-space-limit") {
        std::uint64_t value = 0;
        const auto result =
                std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec == std::errc::result_out_of_range) {
                throw std::overflow_error(std::string(option_name) +
                                          " exceeds uint64_t");
        }
        if (result.ec != std::errc{} || value == 0) {
                throw std::invalid_argument(std::string(option_name) +
                                            " requires a positive size");
        }

        const auto suffix =
                text.substr(static_cast<std::size_t>(result.ptr - text.data()));
        std::uint64_t multiplier = 1;
        if (suffix.empty() || suffix == "B") {
                multiplier = 1;
        } else if (suffix == "KiB") {
                multiplier = 1ULL << 10U;
        } else if (suffix == "MiB") {
                multiplier = 1ULL << 20U;
        } else if (suffix == "GiB") {
                multiplier = 1ULL << 30U;
        } else if (suffix == "TiB") {
                multiplier = 1ULL << 40U;
        } else {
                throw std::invalid_argument(std::string(option_name) +
                                            " has an invalid IEC suffix");
        }
        if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
                throw std::overflow_error(std::string(option_name) +
                                          " exceeds uint64_t");
        }
        return value * multiplier;
}

/**
 * @brief Parses the minimum-two Segment merge fan-in.
 * @param text Decimal integer supplied by the CLI.
 * @return Fan-in representable by std::size_t.
 * @throws std::invalid_argument If the value is malformed or below two.
 */
[[nodiscard]] std::size_t parse_merge_fan_in(std::string_view text) {
        std::size_t value = 0;
        const auto result =
                std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || result.ec != std::errc{} ||
            result.ptr != text.data() + text.size() || value < 2) {
                throw std::invalid_argument(
                        "--merge-fan-in requires an integer of at least two");
        }
        return value;
}

/**
 * @brief Parses a positive worker count without trailing bytes.
 * @param text Decimal integer supplied by the CLI.
 * @return Positive count representable by std::size_t.
 * @throws std::invalid_argument If the value is malformed or zero.
 */
[[nodiscard]] std::size_t parse_worker_threads(std::string_view text) {
        std::size_t value = 0;
        const auto result =
                std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || result.ec != std::errc{} ||
            result.ptr != text.data() + text.size() || value == 0) {
                throw std::invalid_argument(
                        "--threads requires a positive integer");
        }
        return value;
}

/**
 * @brief Parses an exact lowercase resource-profile name.
 * @param text User-provided profile token.
 * @return Corresponding resource profile.
 * @throws std::invalid_argument If the name is unsupported.
 */
[[nodiscard]] ResourceProfile parse_resource_profile(std::string_view text) {
        if (text == "minimal") {
                return ResourceProfile::minimal;
        }
        if (text == "balanced") {
                return ResourceProfile::balanced;
        }
        if (text == "performance") {
                return ResourceProfile::performance;
        }
        throw std::invalid_argument(
                "--profile requires minimal, balanced, or performance");
}

struct ResourceParseState {
        IndexOptions options; ///< Shared resource values parsed so far.
        bool has_profile{};   ///< Whether --profile was already consumed.
};

/**
 * @brief Consumes one shared resource option when the name is recognized.
 * @param option Candidate option name.
 * @param argument Current argv index, advanced when a value is consumed.
 * @param argc Full process argument count.
 * @param argv Writable process argument vector.
 * @param state Resource values and duplicate tracking to update.
 * @return True when option names a shared resource setting.
 * @throws std::invalid_argument If a recognized option is missing or repeated.
 */
bool consume_resource_option(std::string_view option, int &argument, int argc,
                             char *argv[], ResourceParseState &state) {
        const bool recognized = option == "--temporary-directory" ||
                                option == "--temporary-space-limit" ||
                                option == "--merge-fan-in" ||
                                option == "--memory-limit" ||
                                option == "--threads" || option == "--profile";
        if (!recognized) {
                return false;
        }
        if (argument + 1 >= argc) {
                throw std::invalid_argument(std::string(option) +
                                            " requires a value");
        }
        if (option == "--temporary-directory" &&
            !state.options.temporary_directory.has_value()) {
                state.options.temporary_directory = argv[++argument];
        } else if (option == "--temporary-space-limit" &&
            !state.options.temporary_space_limit_bytes.has_value()) {
                state.options.temporary_space_limit_bytes =
                        parse_byte_size(argv[++argument]);
        } else if (option == "--merge-fan-in" &&
                   !state.options.merge_fan_in.has_value()) {
                state.options.merge_fan_in =
                        parse_merge_fan_in(argv[++argument]);
        } else if (option == "--memory-limit" &&
                   !state.options.memory_limit_bytes.has_value()) {
                state.options.memory_limit_bytes =
                        parse_byte_size(argv[++argument], "--memory-limit");
        } else if (option == "--threads" &&
                   !state.options.worker_threads.has_value()) {
                state.options.worker_threads =
                        parse_worker_threads(argv[++argument]);
        } else if (option == "--profile" && !state.has_profile) {
                state.options.profile =
                        parse_resource_profile(argv[++argument]);
                state.has_profile = true;
        } else {
                throw std::invalid_argument(std::string(option) +
                                            " may appear only once");
        }
        return true;
}

/**
 * @brief Parses index/update destination and shared resource options.
 * @param argc Full process argument count.
 * @param argv Writable process argument vector.
 * @return Validated source-command settings.
 * @throws std::invalid_argument If an option is unknown, missing, or repeated.
 */
[[nodiscard]] SourceCommandOptions parse_source_options(int argc,
                                                        char *argv[]) {
        SourceCommandOptions command;
        ResourceParseState resources;
        bool has_index = false;
        for (int argument = 3; argument < argc; ++argument) {
                const std::string_view option(argv[argument]);
                if (option == "--index" && !has_index) {
                        if (argument + 1 >= argc) {
                                throw std::invalid_argument(
                                        "--index requires a value");
                        }
                        command.index_directory = argv[++argument];
                        has_index = true;
                } else if (option == "--index") {
                        throw std::invalid_argument(std::string(option) +
                                                    " may appear only once");
                } else if (!consume_resource_option(option, argument, argc,
                                                    argv, resources)) {
                        throw std::invalid_argument("unknown source option: " +
                                                    std::string(option));
                }
        }
        if (!has_index || command.index_directory.empty()) {
                throw std::invalid_argument(
                        "--index requires one unique directory");
        }
        command.index = std::move(resources.options);
        return command;
}

/**
 * @brief Parses repeated removal Globs and shared resource options.
 * @param argc Full process argument count.
 * @param argv Writable process argument vector.
 * @return Validated remove-command settings with duplicate Globs removed.
 * @throws std::invalid_argument If an option is unknown or incomplete.
 */
[[nodiscard]] RemoveCommandOptions parse_remove_options(int argc,
                                                        char *argv[]) {
        RemoveCommandOptions command;
        ResourceParseState resources;
        for (int argument = 3; argument < argc; ++argument) {
                const std::string_view option(argv[argument]);
                if (option == "--path") {
                        if (argument + 1 >= argc) {
                                throw std::invalid_argument(
                                        "--path requires a value");
                        }
                        command.path_patterns.emplace_back(argv[++argument]);
                } else if (!consume_resource_option(option, argument, argc,
                                                    argv, resources)) {
                        throw std::invalid_argument("unknown remove option: " +
                                                    std::string(option));
                }
        }
        std::sort(command.path_patterns.begin(), command.path_patterns.end());
        command.path_patterns.erase(std::unique(command.path_patterns.begin(),
                                                command.path_patterns.end()),
                                    command.path_patterns.end());
        if (command.path_patterns.empty()) {
                throw std::invalid_argument(
                        "remove requires at least one --path");
        }
        command.index = std::move(resources.options);
        return command;
}

/**
 * @brief Parses compact's shared resource options.
 * @param argc Full process argument count.
 * @param argv Writable process argument vector.
 * @return Validated public writer settings.
 * @throws std::invalid_argument If an option is unknown, missing, or repeated.
 */
[[nodiscard]] IndexOptions parse_compact_options(int argc, char *argv[]) {
        ResourceParseState resources;
        for (int argument = 3; argument < argc; ++argument) {
                const std::string_view option(argv[argument]);
                if (!consume_resource_option(option, argument, argc, argv,
                                             resources)) {
                        throw std::invalid_argument("unknown compact option: " +
                                                    std::string(option));
                }
        }
        return std::move(resources.options);
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
                                        "--source requires one unique "
                                        "directory");
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
void print_rich_result(const SearchHit &result) {
        std::cout << result.path.generic_string();
        if (result.snippet.has_value()) {
                std::cout << ':' << result.snippet->line;
        }
        std::cout << " score=" << format_score(result.score);
        if (result.snippet.has_value() && !result.snippet->text.empty()) {
                std::cout << ' ' << result.snippet->text;
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
void print_json_result(const SearchHit &result, bool include_explanation) {
        const auto line = result.snippet.has_value() ? result.snippet->line : 0;
        const std::string_view snippet = result.snippet.has_value()
                                                 ? result.snippet->text
                                                 : std::string_view{};
        std::cout << "{\"path\":\"" << json_escape(result.path.generic_string())
                  << "\",\"line\":" << line
                  << ",\"score\":" << format_score(result.score)
                  << ",\"snippet\":\"" << json_escape(snippet) << '"';
        if (include_explanation) {
                std::cout << ",\"explanation\":[";
                for (std::size_t index = 0; index < result.explanation.size();
                     ++index) {
                        if (index != 0) {
                                std::cout << ',';
                        }
                        const auto &contribution = result.explanation[index];
                        std::cout
                                << "{\"term\":\""
                                << json_escape(contribution.term)
                                << "\",\"tf\":" << contribution.term_frequency
                                << ",\"df\":" << contribution.document_frequency
                                << ",\"score\":"
                                << format_score(contribution.score) << '}';
                }
                std::cout << ']';
        }
        std::cout << "}\n";
}

/**
 * @brief Returns the stable key-value spelling of an operation outcome.
 * @param outcome Public operation outcome.
 * @return One of unchanged, published, or compacted.
 */
[[nodiscard]] std::string_view outcome_name(IndexOutcome outcome) noexcept {
        switch (outcome) {
        case IndexOutcome::unchanged:
                return "unchanged";
        case IndexOutcome::published:
                return "published";
        case IndexOutcome::compacted:
                return "compacted";
        }
        return "unchanged";
}

/**
 * @brief Returns the stable diagnostic-stage prefix.
 * @param stage Stage that produced a recoverable warning.
 * @return Lowercase prefix used on standard error.
 */
[[nodiscard]] std::string_view
diagnostic_stage_name(DiagnosticStage stage) noexcept {
        switch (stage) {
        case DiagnosticStage::scan:
                return "scan";
        case DiagnosticStage::document:
                return "document";
        case DiagnosticStage::cleanup:
                return "cleanup";
        case DiagnosticStage::maintenance:
                return "maintenance";
        }
        return "maintenance";
}

/**
 * @brief Prints diagnostics and the compact maintenance result protocol.
 * @param result Completed operation result.
 * @param command Command selecting the relevant change counters.
 */
void print_index_result(const IndexResult &result, MaintenanceCommand command) {
        for (const auto &diagnostic : result.diagnostics) {
                std::cerr << diagnostic_stage_name(diagnostic.stage)
                          << " warning: " << diagnostic.path << ": "
                          << diagnostic.message << '\n';
        }
        std::cout << "outcome=" << outcome_name(result.outcome) << '\n'
                  << "revision=" << result.revision << '\n'
                  << "segments=" << result.active_segments << '\n';
        switch (command) {
        case MaintenanceCommand::rebuild:
                std::cout << "indexed=" << result.metrics.indexed_files << '\n'
                          << "failed=" << result.metrics.failed_files << '\n';
                break;
        case MaintenanceCommand::update:
                std::cout << "added=" << result.changes.added << '\n'
                          << "modified=" << result.changes.modified << '\n'
                          << "removed=" << result.changes.removed << '\n'
                          << "unchanged=" << result.changes.unchanged << '\n'
                          << "failed=" << result.metrics.failed_files << '\n';
                break;
        case MaintenanceCommand::remove:
                std::cout << "matched=" << result.changes.matched << '\n';
                break;
        case MaintenanceCommand::compact:
                std::cout << "discarded_records="
                          << result.changes.discarded_records << '\n';
                break;
        }
        std::cout << "memory_peak_bytes=" << result.metrics.peak_memory_bytes
                  << '\n'
                  << "temporary_peak_bytes="
                  << result.metrics.peak_temporary_bytes << '\n'
                  << "warning_count=" << result.diagnostics.size() << '\n';
}

/** @brief Returns status two when an operation produced any warning. */
[[nodiscard]] int index_result_status(const IndexResult &result) noexcept {
        return result.diagnostics.empty() ? 0 : 2;
}

/** @brief Builds and publishes one complete persistent index revision. */
int run_index(const std::filesystem::path &source,
              const SourceCommandOptions &options) {
        const auto result = IndexWriter(options.index_directory, options.index)
                                    .rebuild(source);
        print_index_result(result, MaintenanceCommand::rebuild);
        return index_result_status(result);
}

/** @brief Synchronizes a published index with the current source tree. */
int run_update(const std::filesystem::path &source,
               const SourceCommandOptions &options) {
        const auto result = IndexWriter(options.index_directory, options.index)
                                    .update(source);
        print_index_result(result, MaintenanceCommand::update);
        return index_result_status(result);
}

/** @brief Publishes Tombstones for paths matching one or more Globs. */
int run_remove(const std::filesystem::path &index_directory,
               const RemoveCommandOptions &options) {
        const auto result = IndexWriter(index_directory, options.index)
                                    .remove(options.path_patterns);
        print_index_result(result, MaintenanceCommand::remove);
        return index_result_status(result);
}

/** @brief Compacts all visible records into one canonical Segment. */
int run_compact(const std::filesystem::path &index_directory,
                IndexOptions options) {
        const auto result =
                IndexWriter(index_directory, std::move(options)).compact();
        print_index_result(result, MaintenanceCommand::compact);
        return index_result_status(result);
}

/**
 * @brief Executes an M3 query and renders ranked results.
 * @param index_directory Directory containing the active Segment set.
 * @param expression Complete Boolean query expression.
 * @param options Search and presentation options.
 * @return Zero after a successful query, including an empty result.
 */
int run_query(const std::filesystem::path &index_directory,
              std::string_view expression, const QueryCommandOptions &options) {
        const Searcher engine(index_directory);
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
 * @param index_directory Directory containing the active Segment set.
 * @return Zero after successful validation and output.
 */
int run_stats(const std::filesystem::path &index_directory) {
        const auto stats = validate_index(index_directory);
        std::cout << "documents=" << stats.documents << '\n'
                  << "segments=" << stats.segments << '\n'
                  << "tombstones=" << stats.tombstones << '\n'
                  << "terms=" << stats.terms << '\n'
                  << "postings=" << stats.postings << '\n'
                  << "positions=" << stats.positions << '\n'
                  << "bytes=" << stats.bytes << '\n';
        return 0;
}

/**
 * @brief Fully validates an index and reports a concise success message.
 * @param index_directory Directory containing the active Segment set.
 * @return Zero when every structural and checksum invariant holds.
 */
int run_verify(const std::filesystem::path &index_directory) {
        static_cast<void>(validate_index(index_directory));
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
                if (command == "index" && argc >= 3) {
                        return run_index(argv[2],
                                         parse_source_options(argc, argv));
                }
                if (command == "update" && argc >= 3) {
                        return run_update(argv[2],
                                          parse_source_options(argc, argv));
                }
                if (command == "remove" && argc >= 3) {
                        return run_remove(argv[2],
                                          parse_remove_options(argc, argv));
                }
                if (command == "compact" && argc >= 3) {
                        return run_compact(argv[2],
                                           parse_compact_options(argc, argv));
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
