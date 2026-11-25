#pragma once

#include "platform/win_util.hpp"

#include <filesystem>

namespace ode {

class file_lock {
public:
    explicit file_lock(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        OVERLAPPED range{};
        if (handle_ && !LockFileEx(handle_.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &range)) handle_.reset();
    }
    explicit operator bool() const { return static_cast<bool>(handle_); }

private:
    win::handle handle_;
};

} // namespace ode
