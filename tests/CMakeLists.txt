add_executable(snowseek_unit_tests unit/smoke_test.cpp)
target_link_libraries(snowseek_unit_tests PRIVATE SnowSeek::core)
target_compile_features(snowseek_unit_tests PRIVATE cxx_std_20)
add_test(NAME snowseek.unit COMMAND snowseek_unit_tests)

