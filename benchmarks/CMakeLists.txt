add_executable(snowseek_tokenizer_benchmark tokenizer_benchmark.cpp)
target_link_libraries(snowseek_tokenizer_benchmark PRIVATE SnowSeek::core)
target_compile_features(snowseek_tokenizer_benchmark PRIVATE cxx_std_20)
snowseek_enable_warnings(snowseek_tokenizer_benchmark)

add_executable(snowseek_posting_intersection_benchmark
               posting_intersection_benchmark.cpp)
target_link_libraries(snowseek_posting_intersection_benchmark
                      PRIVATE SnowSeek::core)
target_compile_features(snowseek_posting_intersection_benchmark PRIVATE cxx_std_20)
snowseek_enable_warnings(snowseek_posting_intersection_benchmark)
