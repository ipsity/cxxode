#include "ext/lua/host.hpp"

#include "base/files.hpp"

#include <lauxlib.h>
#include <lualib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <future>
#include <thread>

namespace ode {
namespace {

constexpr std::size_t memory_limit = 8 * 1024 * 1024;
constexpr int hook_interval = 1000;
constexpr int instruction_limit = 4'000'000;
constexpr auto time_limit = std::chrono::seconds(2);

std::string string_path(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

std::string lua_error_text(lua_State* state) {
    return lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "non-string Lua error";
}

int open_libraries(lua_State* state) {
    for (const luaL_Reg lib : {luaL_Reg{LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
                              {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
                              {LUA_UTF8LIBNAME, luaopen_utf8}}) {
        luaL_requiref(state, lib.name, lib.func, 1);
        lua_pop(state, 1);
    }
    // Protected calls can catch the instruction limit, and user finalizers can run with hooks disabled.
    for (const char* name : {"dofile", "loadfile", "load", "pcall", "xpcall", "setmetatable", "print"}) {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }
    lua_newtable(state);
    lua_pushinteger(state, 1);
    lua_setfield(state, -2, "api_version");
    lua_setglobal(state, "ode");
    return 0;
}

} // namespace

struct lua_host::runtime {
    runtime() : worker([this] { work(); }) {}

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
        std::packaged_task task(std::move(fn));
        auto result = task.get_future();
        {
            std::lock_guard lock(mutex);
            pending.emplace_back([task = std::move(task)]() mutable { task(); });
        }
        wake.notify_one();
        return result.get();
    }

    std::vector<std::string> load(const std::vector<std::filesystem::path>& scripts, cancel_token& cancel) {
        return call([&] {
            std::vector<std::string> warnings;
            state = lua_newstate(allocate, this);
            if (!state) return std::vector<std::string>{"Lua: could not create the VM"};
            *static_cast<runtime**>(lua_getextraspace(state)) = this;
            lua_pushcfunction(state, open_libraries);
            if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
                warnings.push_back("Lua: " + lua_error_text(state));
                return warnings;
            }
            for (const auto& path : scripts) {
                if (cancel.cancelled()) break;
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
                int result = luaL_loadbufferx(state, code->data(), code->size(), chunk.c_str(), "t");
                if (result == LUA_OK) result = run(0, 0, cancel);
                if (result != LUA_OK) warnings.push_back(name + ": " + lua_error_text(state));
                lua_settop(state, 0);
            }
            return warnings;
        });
    }

    int run(int args, int results, cancel_token& cancel) {
        current_cancel = &cancel;
        remaining = instruction_limit;
        deadline = std::chrono::steady_clock::now() + time_limit;
        lua_sethook(state, check_limit, LUA_MASKCOUNT, hook_interval);
        const int result = lua_pcall(state, args, results, 0);
        lua_sethook(state, nullptr, 0, 0);
        current_cancel = nullptr;
        return result;
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
            auto next = std::move(pending.front());
            pending.pop_front();
            lock.unlock();
            next();
        }
        if (state) lua_close(state);
    }

    lua_State* state = nullptr;
    std::size_t allocated = 0;
    cancel_token* current_cancel = nullptr;
    int remaining = 0;
    std::chrono::steady_clock::time_point deadline;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::packaged_task<void()>> pending;
    bool stopping = false;
    std::thread worker;
};

lua_host::lua_host() = default;
lua_host::~lua_host() = default;

std::vector<std::string> lua_host::reload(std::vector<std::filesystem::path> scripts, cancel_token& cancel) {
    vm_.reset();
    std::erase_if(scripts, [](const auto& path) {
        std::error_code ec;
        return !std::filesystem::is_regular_file(path, ec);
    });
    if (scripts.empty() || cancel.cancelled()) return {};
    vm_ = std::make_unique<runtime>();
    return vm_->load(scripts, cancel);
}

bool lua_host::active() const { return static_cast<bool>(vm_); }

} // namespace ode
