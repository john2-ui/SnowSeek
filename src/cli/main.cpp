#include "cli/application.hpp"

/**
 * @brief Delegates process execution to the SnowSeek CLI application.
 * @param argc Number of command-line arguments.
 * @param argv Command-line argument vector.
 * @return Exit status produced by the application.
 */
int main(int argc, char *argv[]) { return snowseek::cli::run(argc, argv); }
