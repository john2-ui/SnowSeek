/**
 * @file index_builder.cpp
 * @brief Resolves index-build policies and dispatches builder operations.
 */

#include "index/index_builder.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

namespace snowseek::index {

PersistentBuildOptions persistent_build_options(ResourceProfile profile) {
        PersistentBuildOptions options;
        switch (profile) {
        case ResourceProfile::minimal:
                options.memory_budget_bytes = 128ULL * 1024ULL * 1024ULL;
                options.segment_flush_threshold_bytes =
                        32ULL * 1024ULL * 1024ULL;
                options.worker_thread_count = 1;
                options.merge_fan_in = 4;
                options.in_memory_options.store_positions = false;
                break;
        case ResourceProfile::balanced:
                break;
        case ResourceProfile::performance:
                options.memory_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
                options.segment_flush_threshold_bytes =
                        512ULL * 1024ULL * 1024ULL;
                options.worker_thread_count = std::max<std::size_t>(
                        1, std::thread::hardware_concurrency());
                options.merge_fan_in = 32;
                break;
        }
        return options;
}

IndexBuilder::IndexBuilder(PersistentBuildOptions options)
    : options_(std::move(options)) {
        if (options_.segment_flush_threshold_bytes == 0) {
                throw std::invalid_argument(
                        "segment flush threshold must be positive");
        }
        if (options_.temporary_space_budget_bytes == 0) {
                throw std::invalid_argument(
                        "temporary space budget must be positive");
        }
        if (options_.temporary_directory.has_value()) {
                std::error_code error;
                const bool is_directory = std::filesystem::is_directory(
                        *options_.temporary_directory, error);
                if (error || !is_directory) {
                        throw std::invalid_argument(
                                "temporary directory must be an existing directory: " +
                                options_.temporary_directory->string());
                }
        }
        if (options_.merge_fan_in < 2) {
                throw std::invalid_argument(
                        "merge fan-in must be at least two");
        }
        if (options_.memory_budget_bytes == 0) {
                throw std::invalid_argument("memory budget must be positive");
        }
        if (options_.worker_thread_count == 0) {
                throw std::invalid_argument(
                        "worker thread count must be positive");
        }
        static_cast<void>(InMemoryIndexBuilder(options_.in_memory_options));
}

} // namespace snowseek::index
