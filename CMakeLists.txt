cmake_minimum_required(VERSION 3.24)
project(lay_code VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lay_core STATIC src/core/session.cpp)
target_compile_features(lay_core PUBLIC cxx_std_20)
target_include_directories(lay_core PUBLIC src)

add_library(lay_scripted_model STATIC src/model/scripted_model.cpp)
target_link_libraries(lay_scripted_model PUBLIC lay_core)

add_executable(lay-code app/main.cpp)
target_link_libraries(lay-code PRIVATE lay_scripted_model)

include(CTest)
if(BUILD_TESTING)
    add_executable(session_contract tests/contract/session_contract.cpp)
    target_link_libraries(session_contract PRIVATE lay_scripted_model)
    add_test(NAME session.contract COMMAND session_contract)
    add_test(NAME cli.offline
        COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:lay-code>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/offline.cmake)
endif()
