#include "snowseek/cli/application.hpp"

#include "snowseek/common/version.hpp"

#include <iostream>
#include <string_view>

namespace snowseek::cli {
namespace {

/** @brief Prints command-line usage and the current program version. */
void print_help() {
        std::cout << "SnowSeek " << kVersion << "\n\n"
                  << "Usage:\n"
                  << "  snowseek index <source> --index <dir>\n"
                  << "  snowseek update <source> --index <dir>\n"
                  << "  snowseek query <index> <expression>\n"
                  << "  snowseek stats|verify|compact <index>\n";
}

} // namespace

int run(int argc, char *argv[]) {
        if (argc < 2 || std::string_view(argv[1]) == "--help" ||
            std::string_view(argv[1]) == "-h") {
                print_help();
                return 0;
        }
        if (std::string_view(argv[1]) == "--version") {
                std::cout << kVersion << '\n';
                return 0;
        }
        std::cerr << "snowseek: command framework is ready; command "
                     "implementation is pending\n";
        return 2;
}

} // namespace snowseek::cli
