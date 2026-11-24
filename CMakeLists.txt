cmake_minimum_required(VERSION 3.24)
project(lay VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(LAY_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND LAY_PLATFORM_SOURCES src/platform/http_winhttp.cpp src/platform/process_windows.cpp src/ui/term_windows.cpp)
endif()

add_library(lay_core STATIC
    src/core/version.cpp
    src/core/message.cpp
    src/core/permission.cpp
    src/core/session.cpp
    src/core/agent.cpp
    src/core/prompt.cpp
    src/config/config.cpp
    src/provider/provider.cpp
    src/provider/anthropic.cpp
    src/provider/openai_chat.cpp
    src/provider/openai_responses.cpp
    src/tools/files.cpp
    src/tools/edit_tools.cpp
    src/tools/search_tools.cpp
    src/tools/bash_tool.cpp
    src/ui/render.cpp
    src/skills/skill.cpp
    src/skills/make_tools.cpp
    src/commands/command.cpp
    ${LAY_PLATFORM_SOURCES})
target_include_directories(lay_core PUBLIC src)
target_include_directories(lay_core SYSTEM PUBLIC third_party)
target_compile_definitions(lay_core PUBLIC LAY_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_compile_definitions(lay_core PUBLIC UNICODE _UNICODE)
    target_link_libraries(lay_core PUBLIC winhttp advapi32 shell32)
endif()

add_executable(lay src/main.cpp
    src/app/app.cpp
    src/app/format.cpp
    src/app/commands.cpp
    src/app/interactive.cpp
    src/app/interactive_events.cpp
    src/app/interactive_input.cpp
    src/app/interactive_view.cpp
    src/app/interactive_files.cpp)
target_link_libraries(lay PRIVATE lay_core)
if(MINGW)
    target_link_options(lay PRIVATE -static)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(lay_tests
        tests/main.cpp
        tests/core/version_test.cpp
        tests/core/agent_test.cpp
        tests/provider/provider_test.cpp
        tests/tools/tools_test.cpp
        tests/ui/ui_test.cpp
        tests/skills/skill_test.cpp
        tests/commands/command_test.cpp)
    target_include_directories(lay_tests SYSTEM PRIVATE third_party/doctest)
    target_link_libraries(lay_tests PRIVATE lay_core)
    if(MINGW)
        target_link_options(lay_tests PRIVATE -static)
    endif()
    add_test(NAME unit COMMAND lay_tests)
    add_test(NAME cli.version COMMAND lay --version)
    set_tests_properties(cli.version PROPERTIES PASS_REGULAR_EXPRESSION "^lay ${PROJECT_VERSION}")
endif()
