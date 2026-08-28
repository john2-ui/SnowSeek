add_executable(snowseek_tokenizer_benchmark tokenizer_benchmark.cpp)
target_link_libraries(snowseek_tokenizer_benchmark PRIVATE SnowSeek::core)
target_include_directories(snowseek_tokenizer_benchmark
                           PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(snowseek_tokenizer_benchmark PRIVATE cxx_std_20)
snowseek_enable_warnings(snowseek_tokenizer_benchmark)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(snowseek_index_builder_benchmark index_builder_benchmark.cpp)
    target_link_libraries(snowseek_index_builder_benchmark PRIVATE SnowSeek::core)
    target_include_directories(snowseek_index_builder_benchmark
                               PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(snowseek_index_builder_benchmark PRIVATE cxx_std_20)
    snowseek_enable_warnings(snowseek_index_builder_benchmark)
    if(SNOWSEEK_BUILD_TESTS)
        add_test(NAME snowseek.index_builder_benchmark_smoke
            COMMAND snowseek_index_builder_benchmark
                --files 2
                --bytes-per-file 1024
                --vocabulary 16
                --samples 2
                --changed-files 1)
    endif()
endif()
