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
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <memory>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace lay::platform {
namespace {

#ifdef _WIN32

HANDLE native(std::intptr_t value) { return reinterpret_cast<HANDLE>(value); }

bool basename(std::string_view name);

NTSTATUS open_relative(HANDLE parent, std::wstring_view value, ACCESS_MASK access,
                       ULONG share, ULONG disposition, ULONG options, HANDLE& opened) {
    UNICODE_STRING target{};
    target.Buffer = const_cast<wchar_t*>(value.data());
    target.Length = static_cast<USHORT>(value.size() * sizeof(wchar_t));
    target.MaximumLength = target.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &target;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK status{};
    return NtCreateFile(&opened, access, &attributes, &status, nullptr, FILE_ATTRIBUTE_NORMAL, share,
        disposition, FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | options, nullptr, 0);
}

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

std::wstring absolute_directory(HANDLE handle) {
    std::wstring path(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
                                                   VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
    if (!length || length >= path.size()) {
        throw std::runtime_error("could not resolve the parent directory handle");
    }
    path.resize(length);
    if (!path.ends_with(L'\\')) {
        path += L'\\';
    }
    return path;
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

bool basename(std::string_view name) {
    return !name.empty() && name.size() <= 1020 && name != "." && name != ".." &&
        name.find_first_of(std::string_view("/\\:\0", 4)) == std::string_view::npos;
}

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
    if (!directory_ || !basename(name)) {
        return {};
    }
#ifdef _WIN32
    const auto value = path_from_utf8(name).wstring();
    HANDLE opened = INVALID_HANDLE_VALUE;
    const auto result = open_relative(native(handle_), value, FILE_GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, opened);
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

Node Node::duplicate() const {
#ifdef _WIN32
    HANDLE copy = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), native(handle_), GetCurrentProcess(), &copy,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return {};
    }
    return Node(reinterpret_cast<std::intptr_t>(copy), directory_);
#else
    return Node(::fcntl(native(handle_), F_DUPFD_CLOEXEC, 0), directory_);
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
#ifdef _WIN32
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(native(handle_), start, nullptr, FILE_BEGIN)) {
        throw std::runtime_error("could not seek to the start of the file");
    }
#else
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

std::string Node::identity() const {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(native(handle_), &info)) {
        throw std::runtime_error("could not identify the filesystem object");
    }
    return std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) +
           ":" + std::to_string(info.nFileIndexLow);
#else
    struct stat info{};
    if (::fstat(native(handle_), &info) != 0) {
        throw std::runtime_error("could not identify the filesystem object");
    }
    return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#endif
}

std::filesystem::path Node::current_path() const {
#ifdef _WIN32
    auto value = absolute_directory(native(handle_));
    if (value.starts_with(L"\\\\?\\") && value.size() >= 7 && value[5] == L':') {
        value.erase(0, 4);
    }
    return std::filesystem::path(value).lexically_normal();
#else
    return std::filesystem::canonical("/proc/self/fd/" + std::to_string(handle_));
#endif
}

bool Node::child_exists(std::string_view name) const {
    if (!directory_ || !basename(name)) {
        return true;
    }
#ifdef _WIN32
    HANDLE opened = INVALID_HANDLE_VALUE;
    const auto status = open_relative(native(handle_), path_from_utf8(name).wstring(),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN, 0, opened);
    if (status >= 0) {
        CloseHandle(opened);
        return true;
    }
    constexpr auto name_not_found = static_cast<NTSTATUS>(0xc0000034UL);
    return status != name_not_found;
#else
    const std::string value(name);
    struct stat info{};
    return ::fstatat(native(handle_), value.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT;
#endif
}

bool Node::editable() const {
    if (!*this || directory_) {
        return false;
    }
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    if (!supported(native(handle_), info) ||
        (info.dwFileAttributes & ~(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL))) {
        return false;
    }
    alignas(FILE_STREAM_INFO) char buffer[4096];
    if (!GetFileInformationByHandleEx(native(handle_), FileStreamInfo, buffer, sizeof(buffer))) {
        return false;
    }
    const auto& stream = *reinterpret_cast<const FILE_STREAM_INFO*>(buffer);
    return stream.NextEntryOffset == 0 &&
        std::wstring_view(stream.StreamName, stream.StreamNameLength / sizeof(wchar_t)) == L"::$DATA";
#else
    Node input(reopen(handle_, 0), false);
    struct stat info{};
    return input && ::fstat(native(input.handle_), &info) == 0 &&
        S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && info.st_nlink == 1 &&
        (info.st_mode & S_IWUSR) && !(info.st_mode & 07000) && ::flistxattr(native(input.handle_), nullptr, 0) == 0;
#endif
}

Replacement Node::replace(std::string_view name, std::string_view temporary, std::string_view backup,
                          std::string_view contents, bool create, const std::function<bool()>& verify) const {
    using Status = Replacement::Status;
    if (!directory_ || !basename(name) || !basename(temporary) ||
        (!backup.empty() && !basename(backup)) || child_exists(temporary) ||
        (!backup.empty() && child_exists(backup))) {
        return {Status::failed, "could not reserve edit staging names"};
    }
#ifdef _WIN32
    HANDLE opened = INVALID_HANDLE_VALUE;
    if (open_relative(native(handle_), path_from_utf8(temporary).wstring(), FILE_GENERIC_WRITE,
                      0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, opened) < 0) {
        return {Status::failed, "could not create the edit temporary file (access or space unavailable)"};
    }
    DWORD written = 0;
    const auto valid = WriteFile(opened, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr)
        && written == contents.size() && FlushFileBuffers(opened);
    const auto write_error = GetLastError();
    CloseHandle(opened);
    const auto directory = absolute_directory(native(handle_));
    const auto stage = directory + path_from_utf8(temporary).wstring();
    const auto target = directory + path_from_utf8(name).wstring();
    const auto old = directory + path_from_utf8(backup).wstring();
    if (!valid || !verify()) {
        if (!DeleteFileW(stage.c_str())) {
            return {Status::unknown, "temporary file could not be removed"};
        }
        return {Status::failed, valid ? "file changed before replacement" :
            "temporary write or flush failed (Windows error " + std::to_string(write_error) + ")"};
    }
    if (create) {
        if (MoveFileExW(stage.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return {Status::applied, {}};
        }
        const auto error = GetLastError();
        if ((error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) && DeleteFileW(stage.c_str())) {
            return {Status::failed, "target already exists"};
        }
        return {Status::unknown, "creation did not complete (Windows error " + std::to_string(error) + ")"};
    }
    if (!ReplaceFileW(target.c_str(), stage.c_str(), old.c_str(), 0, nullptr, nullptr)) {
        return {Status::unknown, "replacement did not complete (Windows error " + std::to_string(GetLastError()) + ")"};
    }
    return {Status::applied, {}};
#else
    const auto parent = native(handle_);
    const std::string stage(temporary);
    const std::string target(name);
    Node file(::openat(parent, stage.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600), false);
    if (!file) {
        return {Status::failed, std::string("could not create edit temporary file: ") + std::strerror(errno)};
    }
    const auto discard = [&](std::string detail) {
        if (::unlinkat(parent, stage.c_str(), 0) != 0) {
            return Replacement{Status::unknown, "temporary file could not be removed"};
        }
        return Replacement{Status::failed, std::move(detail)};
    };
    const auto fd = native(file.handle_);
    if (!create) {
        const auto original = child(name);
        struct stat source{}, staging{};
        if (!original.editable() || ::fstat(native(original.handle_), &source) != 0 ||
            ::fstat(fd, &staging) != 0 ||
            (source.st_gid != staging.st_gid && ::fchown(fd, -1, source.st_gid) != 0) ||
            ::fchmod(fd, source.st_mode & 0777) != 0) {
            return discard("file metadata could not be preserved");
        }
    }
    while (!contents.empty()) {
        const auto count = ::write(fd, contents.data(), contents.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return discard(std::string("temporary write failed: ") + std::strerror(errno));
        }
        contents.remove_prefix(static_cast<std::size_t>(count));
    }
    if (::fsync(fd) != 0) {
        return discard(std::string("temporary flush failed: ") + std::strerror(errno));
    }
    if (!verify()) {
        return discard("file changed before replacement");
    }
    const auto renamed = create
        ? ::syscall(SYS_renameat2, parent, stage.c_str(), parent, target.c_str(), RENAME_NOREPLACE)
        : ::renameat(parent, stage.c_str(), parent, target.c_str());
    if (renamed != 0) {
        return discard(std::string("replacement failed: ") + std::strerror(errno));
    }
    Node directory(reopen(handle_, O_DIRECTORY), true);
    if (!directory || ::fsync(native(directory.handle_)) != 0) {
        return {Status::unknown, "replacement observed but parent directory flush failed"};
    }
    return {Status::applied, {}};
#endif
}

void Node::remove_backup(std::string_view name) const {
#ifdef _WIN32
    if (basename(name)) {
        const auto path = absolute_directory(native(handle_)) + path_from_utf8(name).wstring();
        if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            throw std::runtime_error("could not remove the committed edit backup");
        }
    }
#else
    (void)name;
#endif
}

} // namespace lay::platform
