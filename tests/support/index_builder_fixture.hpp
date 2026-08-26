#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace snowseek::test {

/** @brief Owns a unique temporary directory for one test scope. */
class TemporaryDirectory {
      public:
        /**
         * @brief Creates an empty unique directory below the system temporary
         * directory.
         * @throws std::runtime_error If no unique directory can be created.
         */
        TemporaryDirectory() {
                const auto seed = std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count();
                const auto base = std::filesystem::temp_directory_path();
                for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
                        path_ = base / ("snowseek-index-builder-test-" +
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

        /** @brief Removes the owned directory and its contents. */
        ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
        }

        /**
         * @brief Returns the owned directory path.
         * @return Stable path reference valid for this fixture's lifetime.
         */
        [[nodiscard]] const std::filesystem::path &path() const {
                return path_;
        }

      private:
        std::filesystem::path path_;
};

/**
 * @brief Writes binary fixture contents to a path.
 * @param path Destination fixture path.
 * @param contents Bytes to write.
 * @throws std::runtime_error If the fixture cannot be written.
 */
inline void write_file(const std::filesystem::path &path,
                       std::string_view contents) {
        std::ofstream output(path, std::ios::binary);
        output.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        if (!output) {
                throw std::runtime_error("failed to write builder test file");
        }
}

/**
 * @brief Reads a complete Segment fixture for deterministic comparison.
 * @param path Existing Segment path.
 * @return Complete binary contents.
 */
[[nodiscard]] inline std::string read_file(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
}

} // namespace snowseek::test
