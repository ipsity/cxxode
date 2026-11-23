cmake_minimum_required(VERSION 3.24)
project(lay_code VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(LAY_WITH_ANTHROPIC "Build the workspace and online provider modes" OFF)
option(LAY_BENCHMARK_METRICS "Enable development-only local benchmark timing" OFF)

add_library(lay_core STATIC src/core/session.cpp src/core/tools.cpp src/core/process.cpp)
target_compile_features(lay_core PUBLIC cxx_std_20)
target_include_directories(lay_core PUBLIC src)

add_library(lay_scripted_model STATIC src/model/scripted_model.cpp)
target_link_libraries(lay_scripted_model PUBLIC lay_core)

add_library(lay_tui STATIC app/tui.cpp app/tui_terminal.cpp app/tui_workspace.cpp)
target_link_libraries(lay_tui PRIVATE lay_core)

add_executable(lay-code app/main.cpp)
target_link_libraries(lay-code PRIVATE lay_scripted_model lay_tui)

if(LAY_WITH_ANTHROPIC)
    find_package(CURL 7.85 REQUIRED)
    find_package(nlohmann_json 3.11 REQUIRED CONFIG)
    find_package(SQLite3 3.37 REQUIRED)

    add_library(lay_messages STATIC src/model/provider_http.cpp src/model/provider_config.cpp
                                    src/model/provider_credentials.cpp src/model/messages_codec.cpp)
    target_link_libraries(lay_messages PUBLIC lay_core PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
    if(WIN32)
        target_link_libraries(lay_messages PRIVATE advapi32)
    endif()
    add_library(lay_workspace STATIC src/tools/read_only.cpp src/tools/citations.cpp src/tools/edit.cpp src/tools/run.cpp
                                     src/platform/workspace.cpp src/platform/crypto.cpp src/platform/process.cpp)
    target_link_libraries(lay_workspace PUBLIC lay_core)
    add_library(lay_store STATIC src/store/session_store.cpp)
    target_link_libraries(lay_store PUBLIC lay_core PRIVATE lay_workspace nlohmann_json::nlohmann_json)
    if(LAY_BENCHMARK_METRICS)
        target_compile_definitions(lay_messages PRIVATE LAY_BENCHMARK_METRICS)
        target_compile_definitions(lay_store PRIVATE LAY_BENCHMARK_METRICS)
    endif()
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(lay_store PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(lay_store PRIVATE SQLite::SQLite3)
    endif()
    if(WIN32)
        target_sources(lay_workspace PRIVATE src/platform/process_windows.cpp)
        target_compile_definitions(lay_workspace PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(lay_workspace PRIVATE ntdll bcrypt)
    elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Workspace modes support Windows and Linux")
    else()
        target_sources(lay_workspace PRIVATE src/platform/process_linux.cpp)
        find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
        target_link_libraries(lay_workspace PRIVATE OpenSSL::Crypto)
    endif()
    target_compile_definitions(lay-code PRIVATE LAY_WITH_ANTHROPIC)
    target_link_libraries(lay-code PRIVATE lay_messages lay_workspace lay_store)
    if(MINGW)
        target_link_options(lay-code PRIVATE -municode)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(session_contract tests/contract/session_contract.cpp)
    target_link_libraries(session_contract PRIVATE lay_scripted_model)
    add_test(NAME session.contract COMMAND session_contract)
    add_test(NAME cli.offline
        COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:lay-code>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/offline.cmake)
    if(LAY_WITH_ANTHROPIC)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME tui.terminal COMMAND Python3::Interpreter -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/tui.py $<TARGET_FILE:lay-code>)
        set_tests_properties(tui.terminal PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
        add_test(NAME quality.scoring COMMAND Python3::Interpreter -B
            ${CMAKE_CURRENT_SOURCE_DIR}/bench/test_live_quality.py)
        add_executable(readonly_contract tests/contract/readonly_contract.cpp)
        target_link_libraries(readonly_contract PRIVATE lay_messages lay_workspace)
        if(MINGW)
            target_link_options(readonly_contract PRIVATE -municode)
        endif()
        add_test(NAME readonly.contract COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/readonly_contract.py
            $<TARGET_FILE:readonly_contract> $<TARGET_FILE:lay-code>)
        set_tests_properties(readonly.contract PROPERTIES TIMEOUT 45)
        add_test(NAME providers.contract COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/providers.py $<TARGET_FILE:lay-code> $<TARGET_FILE:edit_fault_driver>)
        set_tests_properties(providers.contract PROPERTIES TIMEOUT 45)
        add_executable(edit_fault_driver app/main.cpp)
        target_compile_definitions(edit_fault_driver PRIVATE LAY_WITH_ANTHROPIC LAY_TEST_HOOKS)
        target_link_libraries(edit_fault_driver PRIVATE lay_scripted_model lay_messages lay_workspace lay_store lay_tui)
        if(MINGW)
            target_link_options(edit_fault_driver PRIVATE -municode)
        endif()
        add_test(NAME editing.recovery COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/editing.py
            $<TARGET_FILE:lay-code> $<TARGET_FILE:edit_fault_driver>)
        set_tests_properties(editing.recovery PROPERTIES TIMEOUT 90)
        add_test(NAME editing.two_files COMMAND Python3::Interpreter -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/two_file_editing.py
            $<TARGET_FILE:lay-code> $<TARGET_FILE:edit_fault_driver>)
        set_tests_properties(editing.two_files PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
        add_test(NAME editing.supplement COMMAND Python3::Interpreter -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/supplement.py
            $<TARGET_FILE:lay-code> $<TARGET_FILE:edit_fault_driver>)
        set_tests_properties(editing.supplement PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
        add_test(NAME editing.supplement_live COMMAND Python3::Interpreter -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/supplement_live.py
            $<TARGET_FILE:lay-code> $<TARGET_FILE:edit_fault_driver>)
        set_tests_properties(editing.supplement_live PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        add_executable(process_contract tests/contract/process_contract.cpp)
        target_link_libraries(process_contract PRIVATE lay_workspace nlohmann_json::nlohmann_json)
        if(MINGW)
            target_link_options(process_contract PRIVATE -municode)
        endif()
        add_test(NAME verification.contract COMMAND Python3::Interpreter -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/verification.py
            $<TARGET_FILE:lay-code> $<TARGET_FILE:edit_fault_driver> $<TARGET_FILE:process_contract>
            ${CMAKE_COMMAND} ${CMAKE_CTEST_COMMAND})
        set_tests_properties(verification.contract PROPERTIES TIMEOUT 90)
        add_test(NAME tui.workspace COMMAND Python3::Interpreter -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/tui_workspace.py
            $<TARGET_FILE:lay-code> $<TARGET_FILE:edit_fault_driver> ${CMAKE_COMMAND} ${CMAKE_CTEST_COMMAND})
        set_tests_properties(tui.workspace PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
        add_executable(context_contract tests/contract/context_contract.cpp)
        target_link_libraries(context_contract PRIVATE lay_messages nlohmann_json::nlohmann_json)
        add_test(NAME context.contract COMMAND context_contract)
    endif()
endif()
