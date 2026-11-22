#include "platform/workspace.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <memory>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace lay::platform {
namespace {

#ifdef _WIN32

HANDLE native(std::intptr_t value) { return reinterpret_cast<HANDLE>(value); }

bool supported(HANDLE handle, BY_HANDLE_FILE_INFORMATION& info) {
    return GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE |
                                   FILE_ATTRIBUTE_OFFLINE)) &&
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nNumberOfLinks == 1);
}

bool same_name(HANDLE handle, std::wstring_view requested) {
    std::wstring path(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
                                                   VOLUME_NAME_NONE | FILE_NAME_NORMALIZED);
    if (!length || length >= path.size()) {
        return false;
    }
    path.resize(length);
    const auto final = std::wstring_view(path).substr(path.find_last_of(L'\\') + 1);
    return CompareStringOrdinal(final.data(), static_cast<int>(final.size()), requested.data(),
                                static_cast<int>(requested.size()), FALSE) == CSTR_EQUAL;
}

std::string to_utf8(std::wstring_view name) {
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(),
                                           static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string bytes(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
                             bytes.data(), length, nullptr, nullptr)) {
        return {};
    }
    return bytes;
}

#else

int native(std::intptr_t value) { return static_cast<int>(value); }

int reopen(std::intptr_t handle, int flags) {
    const auto path = "/proc/self/fd/" + std::to_string(handle);
    return ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | flags);
}

#endif

} // namespace

bool path_equal(std::string_view first, std::string_view second) {
#ifdef _WIN32
    const auto a = path_from_utf8(first).wstring();
    const auto b = path_from_utf8(second).wstring();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
#else
    return first == second;
#endif
}

Node::Node(Node&& other) noexcept
    : handle_(std::exchange(other.handle_, -1)), directory_(other.directory_) {}

Node& Node::operator=(Node&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, -1);
        directory_ = other.directory_;
    }
    return *this;
}

Node::~Node() { close(); }

void Node::close() {
    if (*this) {
#ifdef _WIN32
        CloseHandle(native(handle_));
#else
        ::close(native(handle_));
#endif
        handle_ = -1;
    }
}

Node Node::root(const std::filesystem::path& selected) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(selected, error);
    if (error) {
        throw std::runtime_error("could not resolve the selected workspace");
    }
#ifdef _WIN32
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumePathNameW(canonical.c_str(), volume, MAX_PATH) || GetDriveTypeW(volume) != DRIVE_FIXED) {
        throw std::runtime_error("workspace must be on a local fixed volume");
    }
    const auto handle = CreateFileW(canonical.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Node root(reinterpret_cast<std::intptr_t>(handle), true);
    BY_HANDLE_FILE_INFORMATION info{};
    wchar_t filesystem[16]{};
    if (!root || !supported(handle, info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 16) ||
        std::wstring_view(filesystem) != L"NTFS") {
        throw std::runtime_error("could not open a local NTFS workspace directory");
    }
    return root;
#else
    Node root(::open(canonical.c_str(), O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW), true);
    if (!root) {
        throw std::runtime_error("could not open the selected workspace directory");
    }
    // Probe the same kernel path-resolution contract used for every child.
    const open_how how{O_PATH | O_CLOEXEC | O_DIRECTORY,
                      0, RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV};
    Node probe(::syscall(SYS_openat2, native(root.handle_), ".", &how, sizeof(how)), true);
    if (!probe || ::access("/proc/self/fd", X_OK) != 0) {
        throw std::runtime_error("workspace access requires Linux openat2 and /proc/self/fd");
    }
    return root;
#endif
}

Node Node::child(std::string_view name) const {
    if (!directory_ || name.empty() || name == "." || name == ".." ||
        name.find_first_of(std::string_view("/\\:\0", 4)) != std::string_view::npos) {
        return {};
    }
#ifdef _WIN32
    const auto value = path_from_utf8(name).wstring();
    UNICODE_STRING target{};
    target.Buffer = const_cast<wchar_t*>(value.data());
    target.Length = static_cast<USHORT>(value.size() * sizeof(wchar_t));
    target.MaximumLength = target.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = native(handle_);
    attributes.ObjectName = &target;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK status{};
    HANDLE opened = INVALID_HANDLE_VALUE;
    const auto result = NtCreateFile(&opened, FILE_GENERIC_READ, &attributes, &status, nullptr, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT,
        nullptr, 0);
    if (result < 0) {
        return {};
    }
    Node node(reinterpret_cast<std::intptr_t>(opened), false);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!supported(opened, info) || !same_name(opened, value)) {
        return {};
    }
    node.directory_ = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return node;
#else
    const std::string value(name);
    const open_how how{O_PATH | O_CLOEXEC | O_NOFOLLOW,
                      0, RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV};
    Node node(::syscall(SYS_openat2, native(handle_), value.c_str(), &how, sizeof(how)), false);
    struct stat info{};
    if (!node || ::fstat(native(node.handle_), &info) != 0) {
        return {};
    }
    node.directory_ = S_ISDIR(info.st_mode);
    if (!node.directory_ && (!S_ISREG(info.st_mode) || info.st_nlink != 1)) {
        return {};
    }
    return node;
#endif
}

Entries Node::entries(std::size_t limit) const {
    Entries result;
#ifdef _WIN32
    alignas(FILE_ID_BOTH_DIR_INFO) char buffer[16 * 1024];
    auto info_class = FileIdBothDirectoryRestartInfo;
    const auto handle = native(handle_);
    while (GetFileInformationByHandleEx(handle, info_class, buffer, sizeof(buffer))) {
        info_class = FileIdBothDirectoryInfo;
        for (auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer);;) {
            const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                if (result.items.size() == limit) {
                    result.truncated = true;
                    return result;
                }
                const auto flags = entry->FileAttributes;
                result.items.push_back({to_utf8(name), (flags & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    !(flags & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_OFFLINE))});
            }
            if (!entry->NextEntryOffset) {
                break;
            }
            entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
                reinterpret_cast<const char*>(entry) + entry->NextEntryOffset);
        }
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        throw std::runtime_error("could not enumerate directory");
    }
#else
    const auto fd = reopen(handle_, O_DIRECTORY);
    DIR* opened = fd == -1 ? nullptr : ::fdopendir(fd);
    if (!opened) {
        if (fd != -1) {
            ::close(fd);
        }
        throw std::runtime_error("could not enumerate directory");
    }
    const auto closer = [](DIR* directory) { ::closedir(directory); };
    std::unique_ptr<DIR, decltype(closer)> directory(opened, closer);
    while (true) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) {
            if (errno) {
                throw std::runtime_error("could not enumerate directory");
            }
            break;
        }
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        if (result.items.size() == limit) {
            result.truncated = true;
            break;
        }
        result.items.push_back({std::string(name), entry->d_type == DT_DIR,
            entry->d_type == DT_DIR || entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN});
    }
#endif
    return result;
}

std::string Node::read(std::size_t limit) const {
    std::string bytes;
    char buffer[8192];
#ifndef _WIN32
    Node input(reopen(handle_, 0), false);
    if (!input) {
        throw std::runtime_error("could not open file for reading");
    }
#endif
    while (bytes.size() < limit) {
        const auto wanted = std::min(sizeof(buffer), limit - bytes.size());
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(native(handle_), buffer, static_cast<DWORD>(wanted), &got, nullptr)) {
            throw std::runtime_error("could not read file");
        }
#else
        const auto got = ::read(native(input.handle_), buffer, wanted);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("could not read file");
        }
#endif
        if (got == 0) {
            break;
        }
        bytes.append(buffer, static_cast<std::size_t>(got));
    }
    return bytes;
}

} // namespace lay::platform
