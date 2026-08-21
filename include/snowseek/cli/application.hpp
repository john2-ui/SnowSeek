#pragma once

namespace snowseek::cli {

/**
 * @brief Runs the SnowSeek command-line application.
 * @param argc Number of command-line arguments, including the executable name.
 * @param argv Argument vector whose entries remain valid for the call.
 * @return Process exit code: zero for handled informational commands and a
 * nonzero value for unsupported or failed commands.
 */
int run(int argc, char *argv[]);

} // namespace snowseek::cli
