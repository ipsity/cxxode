cmake_minimum_required(VERSION 3.24)
project(lay_code_empty_config_fixture NONE)

enable_testing()
add_test(NAME empty_config COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/check.cmake")
