cmake_minimum_required(VERSION 3.16)

project(SnowSeek VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SNOWSEEK_BUILD_TESTS "Build SnowSeek tests" ON)
option(SNOWSEEK_BUILD_BENCHMARKS "Build SnowSeek benchmarks" OFF)
option(SNOWSEEK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(snowseek_enable_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
        $<$<AND:$<BOOL:${SNOWSEEK_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>
    )
endfunction()

add_library(snowseek_core
    src/analysis/tokenizer.cpp
    src/document/document_store.cpp
    src/document/text_reader.cpp
    src/filesystem/scanner.cpp
    src/index/index.cpp
    src/index/index_builder.cpp
    src/query/posting_operations.cpp
    src/query/query_parser.cpp
    src/query/query_engine.cpp
    src/ranking/bm25.cpp
    src/storage/binary_codec.cpp
    src/storage/checksum.cpp
    src/storage/index_header.cpp
    src/storage/index_file.cpp
    src/storage/segment_merge.cpp
    src/storage/index_validation.cpp
)
add_library(SnowSeek::core ALIAS snowseek_core)
target_link_libraries(snowseek_core PUBLIC Threads::Threads)

target_compile_features(snowseek_core PUBLIC cxx_std_20)
target_include_directories(snowseek_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
snowseek_enable_warnings(snowseek_core)

add_executable(snowseek src/cli/main.cpp src/cli/application.cpp)
target_link_libraries(snowseek PRIVATE SnowSeek::core)
target_compile_features(snowseek PRIVATE cxx_std_20)
snowseek_enable_warnings(snowseek)

if(SNOWSEEK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SNOWSEEK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
