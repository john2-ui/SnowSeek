function(add_snowseek_test name source)
    set(target "snowseek_${name}_tests")
    add_executable(${target} ${source} ${ARGN})
    target_link_libraries(${target} PRIVATE SnowSeek::core)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_compile_features(${target} PRIVATE cxx_std_20)
    snowseek_enable_warnings(${target})
    add_test(NAME "snowseek.${name}" COMMAND ${target})
endfunction()

add_snowseek_test(tokenizer unit/tokenizer_test.cpp)
add_snowseek_test(scanner unit/scanner_test.cpp)
add_snowseek_test(query_parser unit/query_parser_test.cpp)
add_snowseek_test(bm25 unit/bm25_test.cpp)
add_snowseek_test(binary_codec unit/binary_codec_test.cpp)
add_snowseek_test(checksum unit/checksum_test.cpp)
add_snowseek_test(index_header unit/index_header_test.cpp)
add_snowseek_test(index_manifest unit/index_manifest_test.cpp)
add_snowseek_test(index_file integration/index_file_test.cpp)
add_snowseek_test(index_file_validation
    integration/index_file_validation_test.cpp)
add_snowseek_test(index_directory integration/index_directory_test.cpp)
add_snowseek_test(index_directory_visibility
    integration/index_directory_visibility_test.cpp)
add_snowseek_test(text_reader unit/text_reader_test.cpp)
add_snowseek_test(text_pipeline integration/text_pipeline_test.cpp)
add_snowseek_test(document_store unit/document_store_test.cpp)
add_snowseek_test(index unit/index_test.cpp)
add_snowseek_test(index_builder integration/index_builder_test.cpp)
add_snowseek_test(index_builder_resources
    integration/index_builder_resource_test.cpp)
add_snowseek_test(index_maintenance integration/index_maintenance_test.cpp)
add_snowseek_test(persistent_query integration/persistent_query_test.cpp)
add_snowseek_test(public_api unit/public_api_test.cpp
    unit/public_index_header.cpp
    unit/public_search_header.cpp
    unit/public_version_header.cpp)
add_snowseek_test(cli integration/cli_test.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/application.cpp)
