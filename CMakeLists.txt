cmake_minimum_required(VERSION 3.24)
project(cxxode VERSION 0.3.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Lua 5.4.9 from lua.org. C++ mode lets errors unwind native RAII objects.
set(LUA_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c
    llex.c lmathlib.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c
    lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c)
list(TRANSFORM LUA_SOURCES PREPEND "third_party/lua/")
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(ode_lua STATIC ${LUA_SOURCES})
target_include_directories(ode_lua SYSTEM PUBLIC third_party/lua)
target_compile_definitions(ode_lua PRIVATE LUA_USE_JUMPTABLE=0)
if(MINGW)
    target_compile_options(ode_lua PRIVATE $<$<CONFIG:Release>:-Os>)
endif()

# One static library per layer. Each links only the layers below it, so a reversed
# dependency fails at link time instead of compiling silently.
add_library(ode_base STATIC
    src/base/files.cpp
    src/base/text.cpp)
add_library(ode_platform STATIC
    src/platform/http_winhttp.cpp
    src/platform/process_windows.cpp
    src/platform/interactive_process.cpp)
add_library(ode_ui STATIC
    src/ui/render.cpp
    src/ui/term_windows.cpp
    src/ui/theme.cpp
    src/ui/welcome.cpp)
add_library(ode_config STATIC
    src/config/config.cpp
    src/config/provider_config.cpp)
add_library(ode_provider STATIC
    src/provider/provider.cpp
    src/provider/message.cpp
    src/provider/anthropic.cpp
    src/provider/openai_chat.cpp
    src/provider/openai_responses.cpp
    src/provider/models.cpp)
add_library(ode_tools STATIC
    src/tools/files.cpp
    src/tools/edit_tools.cpp
    src/tools/search_tools.cpp
    src/tools/bash_tool.cpp)
add_library(ode_core STATIC
    src/core/version.cpp
    src/core/permission.cpp
    src/core/session.cpp
    src/core/agent.cpp
    src/core/prompt.cpp)
add_library(ode_ext STATIC
    src/ext/discovery.cpp
    src/ext/skills.cpp
    src/ext/commands.cpp
    src/ext/hooks.cpp
    src/ext/lua/host.cpp
    src/ext/mcp.cpp
    src/ext/agents.cpp)
add_library(ode_app STATIC
    src/app/app.cpp
    src/app/format.cpp
    src/app/commands.cpp
    src/app/interactive.cpp
    src/app/interactive_events.cpp
    src/app/interactive_input.cpp
    src/app/interactive_view.cpp
    src/app/interactive_files.cpp
    src/app/builtin_commands.cpp
    src/app/command_history.cpp
    src/app/error_handler.cpp
    src/app/input_validator.cpp
    src/app/tool_suggestion.cpp
    src/app/rpc.cpp)

set(ODE_LAYERS ode_base ode_platform ode_ui ode_config ode_provider ode_tools ode_core ode_ext ode_app)
foreach(layer ${ODE_LAYERS})
    target_include_directories(${layer} PUBLIC src)
    target_include_directories(${layer} SYSTEM PUBLIC third_party)
    target_compile_definitions(${layer} PUBLIC CXXODE_VERSION="${PROJECT_VERSION}")
    if(WIN32)
        target_compile_definitions(${layer} PUBLIC UNICODE _UNICODE)
    endif()
endforeach()

target_link_libraries(ode_platform PUBLIC ode_base winhttp)
target_link_libraries(ode_ui PUBLIC ode_base)
target_link_libraries(ode_config PUBLIC ode_base PRIVATE ode_platform)
target_link_libraries(ode_provider PUBLIC ode_config ode_platform)
target_link_libraries(ode_tools PUBLIC ode_base ode_platform)
target_link_libraries(ode_core PUBLIC ode_provider ode_tools)
target_link_libraries(ode_ext PUBLIC ode_core ode_platform PRIVATE ode_lua)
target_link_libraries(ode_app PUBLIC ode_ext ode_ui)

add_executable(ode src/main.cpp)
target_link_libraries(ode PRIVATE ode_app shell32)
if(MINGW)
    target_link_options(ode PRIVATE -static $<$<CONFIG:Release>:-s>)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(cxxode_tests
        tests/main.cpp
        tests/core/version_test.cpp
        tests/core/agent_test.cpp
        tests/app/provider_switch_test.cpp
        tests/app/palette_test.cpp
        tests/provider/provider_test.cpp
        tests/tools/tools_test.cpp
        tests/ui/ui_test.cpp
        tests/ext/agents_test.cpp
        tests/ext/skills_test.cpp
        tests/ext/commands_test.cpp
        tests/ext/hooks_test.cpp
        tests/ext/lua_test.cpp
        tests/ext/mcp_test.cpp)
    target_include_directories(cxxode_tests SYSTEM PRIVATE third_party/doctest)
    target_link_libraries(cxxode_tests PRIVATE ode_app)
    if(MINGW)
        target_link_options(cxxode_tests PRIVATE -static)
    endif()
    add_test(NAME unit COMMAND cxxode_tests)
    add_test(NAME cli.version COMMAND ode --version)
    set_tests_properties(cli.version PROPERTIES PASS_REGULAR_EXPRESSION "^ode ${PROJECT_VERSION}")
endif()
