#include "ext/lua/host.hpp"

#include "base/files.hpp"

#include <lauxlib.h>
#include <lualib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace ode {
namespace {

constexpr std::size_t memory_limit = 8 * 1024 * 1024;
constexpr int hook_interval = 1000;
constexpr int instruction_limit = 4'000'000;
constexpr auto time_limit = std::chrono::seconds(2);
char json_null;
char load_function_key;
constexpr char array_type[] = "ode.json.array";

std::string string_path(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char ch : value) encoded.push_back(static_cast<char8_t>(ch));
    return std::filesystem::path(encoded);
}

std::string lua_error_text(lua_State* state) {
    return lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "non-string Lua error";
}

void open_libraries(lua_State* state) {
    for (const luaL_Reg lib : {luaL_Reg{LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
                              {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
                              {LUA_UTF8LIBNAME, luaopen_utf8}}) {
        luaL_requiref(state, lib.name, lib.func, 1);
        lua_pop(state, 1);
    }
    // Protected calls can catch the instruction limit, and user finalizers can run with hooks disabled.
    lua_pushlightuserdata(state, &load_function_key);
    lua_getglobal(state, "load");
    lua_rawset(state, LUA_REGISTRYINDEX);
    for (const char* name : {"dofile", "loadfile", "load", "pcall", "xpcall", "setmetatable"}) {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }
}

void push_json(lua_State* state, const json& value, int depth = 0) {
    if (depth > 32 || !lua_checkstack(state, 5)) throw std::runtime_error("nested input exceeds Lua's limit");
    if (value.is_object()) {
        lua_newtable(state);
        for (auto it = value.begin(); it != value.end(); ++it) {
            lua_pushlstring(state, it.key().data(), it.key().size());
            push_json(state, it.value(), depth + 1);
            lua_rawset(state, -3);
        }
    } else if (value.is_array()) {
        lua_newtable(state);
        luaL_setmetatable(state, array_type);
        lua_Integer at = 1;
        for (const auto& item : value) {
            push_json(state, item, depth + 1);
            lua_rawseti(state, -2, at++);
        }
    } else if (value.is_string()) {
        const auto& s = value.get_ref<const std::string&>();
        lua_pushlstring(state, s.data(), s.size());
    } else if (value.is_boolean()) {
        lua_pushboolean(state, value.get<bool>());
    } else if (value.is_number_integer() &&
               (!value.is_number_unsigned() || value.get<std::uint64_t>() <= std::numeric_limits<lua_Integer>::max())) {
        lua_pushinteger(state, value.get<lua_Integer>());
    } else if (value.is_number()) {
        lua_pushnumber(state, value.get<lua_Number>());
    } else {
        lua_pushlightuserdata(state, &json_null);
    }
}

std::string read_string(lua_State* state, int i) {
    luaL_checktype(state, i, LUA_TSTRING);
    std::size_t size;
    const char* data = lua_tolstring(state, i, &size);
    return {data, size};
}

json read_json(lua_State* state, int i, std::size_t& bytes, int depth = 0) {
    if (depth > 32 || !lua_checkstack(state, 5) || bytes < 32) throw std::runtime_error("Lua result is too large or deeply nested");
    bytes -= 32;
    i = lua_absindex(state, i);
    switch (lua_type(state, i)) {
        case LUA_TNIL: return nullptr;
        case LUA_TLIGHTUSERDATA:
            if (lua_touserdata(state, i) == &json_null) return nullptr;
            break;
        case LUA_TBOOLEAN: return lua_toboolean(state, i) != 0;
        case LUA_TNUMBER:
            if (lua_isinteger(state, i)) return lua_tointeger(state, i);
            if (const auto n = lua_tonumber(state, i); std::isfinite(n)) return n;
            break;
        case LUA_TSTRING: {
            std::size_t size;
            const char* data = lua_tolstring(state, i, &size);
            if (size > bytes) throw std::runtime_error("Lua result is too large");
            bytes -= size;
            return std::string(data, size);
        }
        case LUA_TTABLE: {
            bool array = false;
            if (lua_getmetatable(state, i)) {
                luaL_getmetatable(state, array_type);
                array = lua_rawequal(state, -1, -2) != 0;
                lua_pop(state, 2);
            }
            const auto length = lua_rawlen(state, i);
            array = array || length != 0;
            auto result = array ? json::array() : json::object();
            if (array) {
                for (std::size_t at = 1; at <= length; ++at) {
                    if (lua_rawgeti(state, i, static_cast<lua_Integer>(at)) == LUA_TNIL) {
                        throw std::runtime_error("Lua arrays must be contiguous");
                    }
                    result.push_back(read_json(state, -1, bytes, depth + 1));
                    lua_pop(state, 1);
                }
            }
            lua_pushnil(state);
            while (lua_next(state, i)) {
                if (array) {
                    const auto key = lua_tointeger(state, -2);
                    if (!lua_isinteger(state, -2) || key < 1 || static_cast<std::size_t>(key) > length) {
                        throw std::runtime_error("Lua arrays must be contiguous");
                    }
                } else {
                    if (lua_type(state, -2) != LUA_TSTRING) throw std::runtime_error("Lua object keys must be strings");
                    auto key = read_json(state, -2, bytes, depth + 1).get<std::string>();
                    result[std::move(key)] = read_json(state, -1, bytes, depth + 1);
                }
                lua_pop(state, 1);
            }
            return result;
        }
        default: break;
    }
    throw std::runtime_error("Lua value cannot be converted to JSON");
}

json read_json(lua_State* state, int i) {
    std::size_t bytes = memory_limit;
    return read_json(state, i, bytes);
}

std::string registered_name(lua_State* state, int i) {
    auto name = read_string(state, i);
    if (name.empty() || name.size() > 64 || name.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
        luaL_error(state, "registration names must use 1-64 letters, digits, underscores or hyphens");
    }
    return name;
}

tool_output result_output(const json& result) {
    if (result.is_null()) return {};
    if (result.is_string()) return {result.get<std::string>(), false, {}, {}};
    if (result.is_object()) {
        return {result.value("text", std::string{}), result.value("is_error", false),
                result.value("summary", std::string{}), result.value("detail", std::string{})};
    }
    return {"Lua callback must return a string, table or nil", true, "Lua", {}};
}

} // namespace

struct lua_host::runtime {
    struct callback { std::string name; int ref; std::size_t status_slot; };
    struct registered_tool { tool definition; int ref; std::size_t status_slot; };
    struct registered_hook { hook_type type; int ref; std::size_t status_slot; };
    struct job {
        std::function<void()> run;
        std::condition_variable done;
        std::exception_ptr error;
        bool finished = false;
    };

    explicit runtime(std::atomic<std::shared_ptr<const std::string>>& status,
                     std::function<void(std::string_view)> print)
        : status(status), print_output(std::move(print)), worker([this] { work(); }) {}

    ~runtime() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
    }

    template<class F>
    auto call(F fn) {
        std::invoke_result_t<F> out;
        dispatch([&] { out = fn(); });
        return out;
    }

    void dispatch(std::function<void()> fn) {
        // A native tool can trigger hooks while a Lua callback is on the stack.
        if (std::this_thread::get_id() == worker.get_id()) return fn();
        job task{std::move(fn), {}, {}};
        std::unique_lock lock(mutex);
        pending.push_back(&task);
        wake.notify_one();
        task.done.wait(lock, [&] { return task.finished; });
        if (task.error) std::rethrow_exception(task.error);
    }

    std::vector<std::string> load(const std::vector<lua_script>& scripts, cancel_token& cancel) {
        return call([&] {
            std::vector<std::string> warnings;
            state = lua_newstate(allocate, this);
            if (!state) return std::vector<std::string>{"Lua: could not create the VM"};
            *static_cast<runtime**>(lua_getextraspace(state)) = this;
            lua_pushcfunction(state, bridge<open_libraries>);
            if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
                warnings.push_back("Lua: " + lua_error_text(state));
                return warnings;
            }
            status_slots.assign(scripts.size(), {});
            for (std::size_t script_index = 0; script_index < scripts.size(); ++script_index) {
                const auto& script = scripts[script_index];
                active_status_slot = script_index;
                const auto& path = script.path;
                const auto name = string_path(path);
                std::error_code ec;
                if (std::filesystem::file_size(path, ec) > memory_limit || ec) {
                    warnings.push_back(name + ": Lua script cannot be read or exceeds the memory limit");
                    continue;
                }
                const auto code = read_file(path);
                if (!code) {
                    warnings.push_back(name + ": Lua script cannot be read");
                    continue;
                }
                const auto chunk = "@" + name;
                const auto tool_count = tools.size();
                const auto hook_count = hooks.size();
                const auto command_count = commands.size();
                const auto key_count = keys.size();
                int result = luaL_loadbufferx(state, code->data(), code->size(), chunk.c_str(), "t");
                loading = true;
                if (result == LUA_OK) {
                    push_script_environment(script, path);
                    result = run(0, 0, cancel);
                }
                loading = false;
                if (result != LUA_OK) {
                    warnings.push_back(name + ": " + lua_error_text(state));
                    discard(tools, tool_count);
                    discard(hooks, hook_count);
                    discard(commands, command_count);
                    command_names.resize(command_count);
                    discard(keys, key_count);
                    status_slots[script_index].clear();
                } else {
                    publish_status();
                }
                status_update.reset();
                lua_settop(state, 0);
            }
            return warnings;
        });
    }

    int run(int args, int results, cancel_token& cancel) {
        const auto previous_cancel = current_cancel;
        const auto previous_remaining = remaining;
        const auto previous_deadline = deadline;
        current_cancel = &cancel;
        remaining = instruction_limit;
        deadline = std::chrono::steady_clock::now() + time_limit;
        lua_sethook(state, check_limit, LUA_MASKCOUNT, hook_interval);
        const int result = lua_pcall(state, args, results, 0);
        lua_sethook(state, previous_cancel ? check_limit : nullptr, previous_cancel ? LUA_MASKCOUNT : 0, hook_interval);
        current_cancel = previous_cancel;
        remaining = previous_remaining;
        deadline = previous_deadline;
        return result;
    }

    struct invocation {
        int ref;
        const json& input;
        json output;
    };

    std::string invoke(int ref, const json& input, json& output, cancel_token& cancel,
                       tool_context* ctx = nullptr, std::size_t status_slot = 0) {
        if (cancel.cancelled()) return "Lua execution cancelled";
        const int top = lua_gettop(state);
        const auto previous = action;
        const auto previous_status_slot = active_status_slot;
        active_status_slot = status_slot;
        auto previous_status = std::exchange(status_update, std::nullopt);
        action = ctx;
        invocation request{ref, input, {}};
        lua_pushcfunction(state, bridge<invoke_callback>);
        lua_pushlightuserdata(state, &request);
        const int result = run(1, 0, cancel);
        action = previous;
        auto error = result == LUA_OK ? std::string{} : lua_error_text(state);
        if (error.empty() && result != LUA_OK) error = "Lua execution failed";
        lua_settop(state, top);
        if (result == LUA_OK && !cancel.cancelled()) publish_status();
        active_status_slot = previous_status_slot;
        status_update = std::move(previous_status);
        output = std::move(request.output);
        return error;
    }

    tool_output invoke_action(int ref, std::size_t status_slot, const json& input, tool_context& ctx) {
        json result;
        if (auto error = invoke(ref, input, result, *ctx.cancel, &ctx, status_slot); !error.empty()) {
            return {"Lua: " + error, true, "Lua", {}};
        }
        try {
            return result_output(result);
        } catch (const json::exception& e) {
            return {std::string("Invalid Lua result: ") + e.what(), true, "Lua", {}};
        }
    }

    static int invoke_callback(runtime&, lua_State* state) {
        auto& request = *static_cast<invocation*>(lua_touserdata(state, 1));
        lua_rawgeti(state, LUA_REGISTRYINDEX, request.ref);
        push_json(state, request.input);
        lua_call(state, 1, 1);
        request.output = read_json(state, -1);
        return 0;
    }

    template<int (*fn)(runtime&, lua_State*)>
    static int bridge(lua_State* state) {
        try {
            return fn(**static_cast<runtime**>(lua_getextraspace(state)), state);
        } catch (const std::exception& e) {
            return luaL_error(state, "%s", e.what());
        }
    }

    static int open_libraries(runtime&, lua_State* state) {
        ode::open_libraries(state);
        lua_pushcfunction(state, bridge<print_values>);
        lua_setglobal(state, "print");
        lua_newtable(state);
        lua_pushliteral(state, "ode.array");
        lua_setfield(state, -2, "__metatable");
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushinteger(state, 1);
        lua_setfield(state, -2, "api_version");
        lua_pushlightuserdata(state, &json_null);
        lua_setfield(state, -2, "null");
        lua_pushcfunction(state, bridge<make_array>);
        lua_setfield(state, -2, "array");
        lua_pushcfunction(state, bridge<add_hook>);
        lua_setfield(state, -2, "on");
        lua_pushcfunction(state, bridge<add_tool>);
        lua_setfield(state, -2, "tool");
        lua_pushcfunction(state, bridge<add_command>);
        lua_setfield(state, -2, "command");
        lua_pushcfunction(state, bridge<exec>);
        lua_setfield(state, -2, "exec");
        lua_newtable(state);
        lua_pushcfunction(state, bridge<read>);
        lua_setfield(state, -2, "read");
        lua_pushcfunction(state, bridge<write>);
        lua_setfield(state, -2, "write");
        lua_pushcfunction(state, bridge<edit>);
        lua_setfield(state, -2, "edit");
        lua_setfield(state, -2, "fs");
        lua_pushcfunction(state, bridge<add_key>);
        lua_setfield(state, -2, "keymap");
        lua_newtable(state);
        lua_pushcfunction(state, bridge<set_status>);
        lua_setfield(state, -2, "set_status");
        lua_setfield(state, -2, "ui");
        lua_setglobal(state, "ode");
        return 0;
    }

    void push_script_environment(const lua_script& script, const std::filesystem::path& path) {
        const int chunk = lua_absindex(state, -1);
        lua_newtable(state);
        const int environment = lua_absindex(state, -1);

        lua_newtable(state);
        lua_pushglobaltable(state);
        lua_setfield(state, -2, "__index");
        lua_setmetatable(state, environment);
        lua_pushvalue(state, environment);
        lua_setfield(state, environment, "_G");

        if (!script.plugin_name.empty()) {
            lua_getglobal(state, "ode");
            const int global_ode = lua_absindex(state, -1);
            lua_newtable(state);
            const int local_ode = lua_absindex(state, -1);
            lua_pushnil(state);
            while (lua_next(state, global_ode)) {
                lua_pushvalue(state, -2);
                lua_pushvalue(state, -2);
                lua_rawset(state, local_ode);
                lua_pop(state, 1);
            }
            lua_newtable(state);
            lua_pushvalue(state, global_ode);
            lua_setfield(state, -2, "__index");
            lua_setmetatable(state, local_ode);
            lua_newtable(state);
            lua_pushlstring(state, script.plugin_name.data(), script.plugin_name.size());
            lua_setfield(state, -2, "name");
            lua_pushlstring(state, script.plugin_version.data(), script.plugin_version.size());
            lua_setfield(state, -2, "version");
            const auto plugin_path = string_path(path.parent_path().parent_path());
            lua_pushlstring(state, plugin_path.data(), plugin_path.size());
            lua_setfield(state, -2, "path");
            lua_setfield(state, local_ode, "plugin");
            lua_setfield(state, environment, "ode");
            lua_pop(state, 1);
        }

        lua_pushvalue(state, environment);
        lua_pushcclosure(state, bridge<load_text>, 1);
        lua_setfield(state, environment, "load");
        const auto directory = string_path(path.parent_path());
        lua_pushlstring(state, directory.data(), directory.size());
        lua_pushvalue(state, environment);
        lua_newtable(state);
        lua_pushcclosure(state, bridge<require_module>, 3);
        lua_setfield(state, environment, "require");
        lua_pushvalue(state, environment);
        if (!lua_setupvalue(state, chunk, 1)) {
            lua_pop(state, 1);
            throw std::runtime_error("Lua chunk has no environment upvalue");
        }
        lua_pop(state, 1);
    }

    static int require_module(runtime& self, lua_State* state) {
        auto module = read_string(state, 1);
        if (module.empty() || module.size() > 128 || module.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos ||
            module.find("..") != std::string::npos || module.front() == '.' || module.back() == '.') {
            return luaL_error(state, "invalid Lua module name");
        }
        for (auto& ch : module) if (ch == '.') ch = '/';

        lua_pushvalue(state, 1);
        lua_rawget(state, lua_upvalueindex(3));
        if (!lua_isnil(state, -1)) return 1;
        lua_pop(state, 1);

        std::size_t directory_size = 0;
        const char* directory = lua_tolstring(state, lua_upvalueindex(1), &directory_size);
        const auto dir = path_from_utf8(std::string_view(directory, directory_size));
        const auto path = dir / path_from_utf8(module + ".lua");
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > memory_limit) return luaL_error(state, "Lua module not found or too large: %s", module.c_str());
        const auto source = read_file(path);
        if (!source) return luaL_error(state, "Lua module cannot be read: %s", module.c_str());
        const auto chunk_name = "@" + string_path(path);
        int result = luaL_loadbufferx(state, source->data(), source->size(), chunk_name.c_str(), "t");
        if (result != LUA_OK) return lua_error(state);

        lua_pushvalue(state, lua_upvalueindex(2));
        if (!lua_setupvalue(state, -2, 1)) return luaL_error(state, "Lua module has no environment upvalue");
        lua_pushvalue(state, 1);
        lua_pushboolean(state, 1);
        lua_rawset(state, lua_upvalueindex(3));
        result = lua_pcall(state, 0, 1, 0);
        if (result != LUA_OK) {
            lua_pushvalue(state, 1);
            lua_pushnil(state);
            lua_rawset(state, lua_upvalueindex(3));
            return lua_error(state);
        }
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            lua_pushboolean(state, 1);
        }
        lua_pushvalue(state, 1);
        lua_pushvalue(state, -2);
        lua_rawset(state, lua_upvalueindex(3));
        (void)self;
        return 1;
    }

    static int load_text(runtime&, lua_State* state) {
        const int original_count = lua_gettop(state);
        lua_pushlightuserdata(state, &load_function_key);
        lua_rawget(state, LUA_REGISTRYINDEX);
        lua_pushvalue(state, 1);
        if (original_count >= 2) lua_pushvalue(state, 2);
        else lua_pushnil(state);
        lua_pushliteral(state, "t");
        if (original_count >= 4) lua_pushvalue(state, 4);
        else lua_pushvalue(state, lua_upvalueindex(1));
        lua_call(state, 4, LUA_MULTRET);
        const int result_count = lua_gettop(state) - original_count;
        lua_rotate(state, 1, -original_count);
        lua_settop(state, result_count);
        return result_count;
    }

    static int print_values(runtime& self, lua_State* state) {
        std::string line;
        const int count = lua_gettop(state);
        for (int i = 1; i <= count; ++i) {
            if (i > 1) line.push_back('\t');
            std::size_t size = 0;
            const char* value = luaL_tolstring(state, i, &size);
            line.append(value, size);
            lua_pop(state, 1);
        }
        if (self.print_output) self.print_output(line);
        return 0;
    }

    static int make_array(runtime&, lua_State* state) {
        if (lua_isnoneornil(state, 1)) lua_newtable(state);
        else {
            luaL_checktype(state, 1, LUA_TTABLE);
            lua_pushvalue(state, 1);
        }
        luaL_setmetatable(state, array_type);
        return 1;
    }

    template<class T>
    void discard(std::vector<T>& list, std::size_t size) {
        while (list.size() > size) {
            luaL_unref(state, LUA_REGISTRYINDEX, list.back().ref);
            list.pop_back();
        }
    }

    void require_loading() const {
        if (!loading) throw std::runtime_error("registrations are only allowed in init.lua");
    }

    static int add_hook(runtime& self, lua_State* state) {
        self.require_loading();
        const auto name = read_string(state, 1);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        constexpr std::pair<std::string_view, hook_type> types[] = {
            {"pre_tool", hook_type::pre_tool_use}, {"post_tool", hook_type::post_tool_use},
            {"prompt", hook_type::user_prompt_submit}, {"stop", hook_type::stop}};
        const auto type = std::ranges::find(types, name, &std::pair<std::string_view, hook_type>::first);
        if (type == std::end(types)) return luaL_error(state, "unknown hook event");
        lua_pushvalue(state, 2);
        self.hooks.push_back({type->second, luaL_ref(state, LUA_REGISTRYINDEX), self.active_status_slot});
        return 0;
    }

    static int add_tool(runtime& self, lua_State* state) {
        self.require_loading();
        luaL_checktype(state, 1, LUA_TTABLE);
        tool definition{};
        lua_getfield(state, 1, "name");
        definition.name = registered_name(state, -1);
        lua_pop(state, 1);
        if (std::ranges::any_of(self.tools, [&](const auto& t) { return t.definition.name == definition.name; })) {
            return luaL_error(state, "tool name is already registered");
        }
        lua_getfield(state, 1, "description");
        definition.description = read_string(state, -1);
        lua_pop(state, 1);
        lua_getfield(state, 1, "schema");
        definition.schema = read_json(state, -1);
        lua_pop(state, 1);
        if (!definition.schema.is_object() || definition.schema.value("type", std::string{}) != "object") {
            return luaL_error(state, "tool schema must describe a JSON object");
        }
        lua_getfield(state, 1, "access");
        const auto access = read_string(state, -1);
        lua_pop(state, 1);
        if (access == "read") definition.access = tool_access::read;
        else if (access == "write") definition.access = tool_access::write;
        else if (access == "execute") definition.access = tool_access::execute;
        else return luaL_error(state, "tool access must be read, write or execute");
        lua_getfield(state, 1, "run");
        luaL_checktype(state, -1, LUA_TFUNCTION);
        self.tools.push_back({std::move(definition), luaL_ref(state, LUA_REGISTRYINDEX), self.active_status_slot});
        return 0;
    }

    static int add_command(runtime& self, lua_State* state) {
        self.require_loading();
        auto name = registered_name(state, 1);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        if (std::ranges::find(self.command_names, name) != self.command_names.end()) {
            return luaL_error(state, "command name is already registered");
        }
        lua_pushvalue(state, 2);
        self.command_names.push_back(name);
        self.commands.push_back({std::move(name), luaL_ref(state, LUA_REGISTRYINDEX), self.active_status_slot});
        return 0;
    }

    static int add_key(runtime& self, lua_State* state) {
        self.require_loading();
        auto name = read_string(state, 1);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        const bool ctrl = name.starts_with("Ctrl+") && name.size() == 6 && name.back() >= 'A' && name.back() <= 'Z';
        constexpr std::string_view reserved_ctrl_keys = "acdeklotuw";
        const bool reserved = ctrl && (name.back() == 'C' || name.back() == 'J' ||
            reserved_ctrl_keys.find(static_cast<char>(std::tolower(static_cast<unsigned char>(name.back())))) !=
                std::string_view::npos);
        constexpr std::string_view function_keys[] = {
            "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        if (reserved || (!ctrl && std::ranges::find(function_keys, name) == std::end(function_keys))) {
            return luaL_error(state, "unsupported or reserved key; use an unassigned Ctrl letter or F1 through F12");
        }
        if (std::ranges::find(self.keys, name, &callback::name) != self.keys.end()) {
            return luaL_error(state, "key is already registered");
        }
        lua_pushvalue(state, 2);
        self.keys.push_back({std::move(name), luaL_ref(state, LUA_REGISTRYINDEX), self.active_status_slot});
        return 0;
    }

    static int set_status(runtime& self, lua_State* state) {
        self.status_update = clip(read_string(state, 1), 512);
        return 0;
    }

    void publish_status() {
        if (!status_update || active_status_slot >= status_slots.size()) return;
        status_slots[active_status_slot] = std::move(*status_update);
        std::string combined;
        for (const auto& value : status_slots) {
            if (value.empty()) continue;
            if (!combined.empty()) combined += " · ";
            combined += value;
        }
        if (combined.empty()) status.store({});
        else status.store(std::make_shared<const std::string>(std::move(combined)));
    }

    int native_tool(std::string_view name, json input) {
        if (!action || !action->invoke_builtin) {
            return luaL_error(state, "file and process operations require a tool or command callback");
        }
        const auto start = std::chrono::steady_clock::now();
        remaining -= hook_interval;
        if (remaining <= 0 || start >= deadline) return luaL_error(state, "Lua execution limit exceeded");
        const auto out = action->invoke_builtin(name, input);
        deadline += std::chrono::steady_clock::now() - start;
        if (current_cancel->cancelled()) return luaL_error(state, "Lua execution cancelled");
        push_json(state, {{"text", out.text}, {"is_error", out.is_error}, {"summary", out.summary}, {"detail", out.detail}});
        return 1;
    }

    static int read(runtime& self, lua_State* state) {
        json in{{"file_path", read_string(state, 1)}};
        if (!lua_isnoneornil(state, 2)) in["offset"] = luaL_checkinteger(state, 2);
        if (!lua_isnoneornil(state, 3)) in["limit"] = luaL_checkinteger(state, 3);
        return self.native_tool("Read", std::move(in));
    }

    static int write(runtime& self, lua_State* state) {
        return self.native_tool("Write", {{"file_path", read_string(state, 1)}, {"content", read_string(state, 2)}});
    }

    static int edit(runtime& self, lua_State* state) {
        if (!lua_isnoneornil(state, 4)) luaL_checktype(state, 4, LUA_TBOOLEAN);
        return self.native_tool("Edit", {{"file_path", read_string(state, 1)}, {"old_string", read_string(state, 2)},
                                {"new_string", read_string(state, 3)}, {"replace_all", lua_toboolean(state, 4) != 0}});
    }

    static int exec(runtime& self, lua_State* state) {
        json in{{"command", read_string(state, 1)}};
        if (!lua_isnoneornil(state, 2)) in["timeout"] = luaL_checkinteger(state, 2);
        return self.native_tool("Bash", std::move(in));
    }

    static void* allocate(void* user, void* ptr, std::size_t old_size, std::size_t new_size) {
        auto& used = static_cast<runtime*>(user)->allocated;
        if (!ptr) old_size = 0;
        if (!new_size) {
            std::free(ptr);
            used -= old_size;
            return nullptr;
        }
        if (new_size > old_size && new_size - old_size > memory_limit - used) return nullptr;
        if (void* resized = std::realloc(ptr, new_size)) {
            used = used - old_size + new_size;
            return resized;
        }
        return nullptr;
    }

    static void check_limit(lua_State* state, lua_Debug*) {
        auto& self = **static_cast<runtime**>(lua_getextraspace(state));
        if (self.current_cancel->cancelled()) luaL_error(state, "Lua execution cancelled");
        self.remaining -= hook_interval;
        if (self.remaining <= 0 || std::chrono::steady_clock::now() >= self.deadline) {
            luaL_error(state, "Lua execution limit exceeded");
        }
    }

    void work() {
        for (;;) {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || !pending.empty(); });
            if (pending.empty()) break;
            auto* next = pending.front();
            pending.pop_front();
            lock.unlock();
            try {
                next->run();
            } catch (...) {
                next->error = std::current_exception();
            }
            lock.lock();
            next->finished = true;
            next->done.notify_one();
        }
        if (state) lua_close(state);
    }

    lua_State* state = nullptr;
    bool loading = false;
    tool_context* action = nullptr;
    std::vector<registered_tool> tools;
    std::vector<registered_hook> hooks;
    std::vector<callback> commands;
    std::vector<std::string> command_names;
    std::vector<callback> keys;
    std::vector<std::string> status_slots;
    std::size_t active_status_slot = 0;
    std::atomic<std::shared_ptr<const std::string>>& status;
    std::function<void(std::string_view)> print_output;
    std::optional<std::string> status_update;
    std::size_t allocated = 0;
    cancel_token* current_cancel = nullptr;
    int remaining = 0;
    std::chrono::steady_clock::time_point deadline;
    std::mutex mutex;
    std::condition_variable wake;
    // The submitting thread keeps each job alive until the worker signals completion.
    std::deque<job*> pending;
    bool stopping = false;
    std::thread worker;
};

lua_host::lua_host(std::function<void(std::string_view)> print) : print_(std::move(print)) {}
lua_host::~lua_host() = default;

std::vector<std::string> lua_host::reload(std::vector<std::filesystem::path> scripts, cancel_token& cancel) {
    std::vector<lua_script> entries;
    entries.reserve(scripts.size());
    for (auto& path : scripts) entries.push_back({std::move(path), {}, {}});
    return reload_scripts(std::move(entries), cancel);
}

std::vector<std::string> lua_host::reload_scripts(std::vector<lua_script> scripts, cancel_token& cancel) {
    vm_.reset();
    status_.store({});
    std::erase_if(scripts, [](const auto& script) {
        std::error_code ec;
        return !std::filesystem::is_regular_file(script.path, ec);
    });
    if (scripts.empty() || cancel.cancelled()) return {};
    vm_ = std::make_unique<runtime>(status_, print_);
    return vm_->load(scripts, cancel);
}

bool lua_host::active() const { return static_cast<bool>(vm_); }

std::vector<tool> lua_host::tools() {
    std::vector<tool> result;
    if (!vm_) return result;
    for (const auto& registered : vm_->tools) {
        auto t = registered.definition;
        t.run = [this, name = t.name](const json& input, tool_context& ctx) -> tool_output {
            if (!vm_) return {"Lua extension is unavailable", true, "Lua", {}};
            return vm_->call([&] {
                const auto registered = std::ranges::find_if(vm_->tools, [&](const auto& r) {
                    return r.definition.name == name;
                });
                if (registered == vm_->tools.end()) return tool_output{"Lua tool is unavailable", true, "Lua", {}};
                return vm_->invoke_action(registered->ref, registered->status_slot, input, ctx);
            });
        };
        result.push_back(std::move(t));
    }
    return result;
}

const std::vector<std::string>& lua_host::commands() const {
    static const std::vector<std::string> empty;
    return vm_ ? vm_->command_names : empty;
}

bool lua_host::has_hooks() const { return vm_ && !vm_->hooks.empty(); }

hook_verdict lua_host::run_hooks(hook_type type, const json& input, cancel_token& cancel,
                                hook_verdict (*parse)(hook_type, const json&)) {
    if (!has_hooks()) return {};
    return vm_->call([&] {
        const auto append = [](std::string& out, const std::string& part) {
            if (part.empty()) return;
            if (!out.empty()) out += '\n';
            out += part;
        };
        hook_verdict merged;
        for (const auto& hook : vm_->hooks) {
            if (cancel.cancelled()) break;
            if (hook.type != type) continue;
            json output;
            if (auto error = vm_->invoke(hook.ref, input, output, cancel, nullptr, hook.status_slot); !error.empty()) {
                append(merged.warning, "Lua hook: " + error);
                continue;
            }
            if (!output.is_null() && !output.is_object()) {
                append(merged.warning, "Lua hook must return a table or nil");
                continue;
            }
            auto result = parse(type, output);
            if (!result.allow) {
                merged.allow = false;
                append(merged.message, result.message);
                if (type != hook_type::post_tool_use) break;
            }
            if (result.tool_permission == hook_verdict::permission::ask ||
                merged.tool_permission == hook_verdict::permission::inherit) {
                merged.tool_permission = result.tool_permission;
            }
            append(merged.additional_context, result.additional_context);
        }
        return merged;
    });
}

tool_output lua_host::run_command(std::string_view name, std::string_view args, tool_context& ctx) {
    if (!vm_) return {"Lua extension is unavailable", true, "Lua", {}};
    return vm_->call([&] {
        const auto cmd = std::ranges::find(vm_->commands, name, &runtime::callback::name);
        if (cmd == vm_->commands.end()) return tool_output{"Lua command is unavailable", true, "Lua", {}};
        return vm_->invoke_action(cmd->ref, cmd->status_slot, args, ctx);
    });
}

bool lua_host::has_key(std::string_view name) const {
    return vm_ && std::ranges::find(vm_->keys, name, &runtime::callback::name) != vm_->keys.end();
}

std::vector<std::string> lua_host::keys() const {
    std::vector<std::string> result;
    if (vm_) {
        for (const auto& key : vm_->keys) result.push_back(key.name);
    }
    return result;
}

lua_key_result lua_host::run_key(std::string_view name, cancel_token& cancel) {
    if (!vm_) return {};
    return vm_->call([&] {
        const auto key = std::ranges::find(vm_->keys, name, &runtime::callback::name);
        if (key == vm_->keys.end()) return lua_key_result{};
        json output;
        auto error = vm_->invoke(key->ref, name, output, cancel, nullptr, key->status_slot);
        if (!error.empty()) return lua_key_result{false, "Lua key " + std::string(name) + ": " + error};
        if (output.is_null() || output.is_boolean()) return lua_key_result{output.is_null() || output.get<bool>(), {}};
        return lua_key_result{false, "Lua key callback must return a boolean or nil"};
    });
}

std::shared_ptr<const std::string> lua_host::status() const { return status_.load(); }

} // namespace ode
