cmake_minimum_required(VERSION 3.16)

project(SnowSeek VERSION 0.1.0 LANGUAGES CXX)

option(SNOWSEEK_BUILD_TESTS "Build SnowSeek tests" ON)
option(SNOWSEEK_BUILD_BENCHMARKS "Build SnowSeek benchmarks" OFF)

add_library(snowseek_core
    src/analysis/tokenizer.cpp
    src/filesystem/scanner.cpp
    src/index/index_builder.cpp
    src/query/query_engine.cpp
    src/ranking/bm25.cpp
    src/storage/index_header.cpp
)
add_library(SnowSeek::core ALIAS snowseek_core)

target_compile_features(snowseek_core PUBLIC cxx_std_20)
target_include_directories(snowseek_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_options(snowseek_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_executable(snowseek src/cli/main.cpp src/cli/application.cpp)
target_link_libraries(snowseek PRIVATE SnowSeek::core)
target_compile_features(snowseek PRIVATE cxx_std_20)

if(SNOWSEEK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SNOWSEEK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
