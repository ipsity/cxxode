#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lay::platform {

struct Entry {
    std::string name;
    bool directory;
    bool supported;
};

struct Entries {
    std::vector<Entry> items;
    bool truncated = false;
};

struct Replacement {
    enum class Status { applied, failed, unknown } status;
    std::string diagnostic;
};

bool path_equal(std::string_view first, std::string_view second);

inline std::filesystem::path path_from_utf8(std::string_view path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

// A Node owns the opened object. Child lookup and enumeration use that handle.
class Node {
public:
    Node() = default;
    Node(Node&& other) noexcept;
    Node& operator=(Node&& other) noexcept;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node();

    static Node root(const std::filesystem::path& selected);
    Node child(std::string_view name) const;
    Node duplicate() const;
    Entries entries(std::size_t limit) const;
    std::string read(std::size_t limit) const;
    std::string identity() const;
    std::filesystem::path current_path() const;
    std::intptr_t descriptor() const { return handle_; }
    bool child_exists(std::string_view name) const;
    bool editable() const;
    Replacement replace(std::string_view name, std::string_view temporary, std::string_view backup,
                        std::string_view contents, bool create, const std::function<bool()>& verify) const;
    void remove_backup(std::string_view name) const;
    explicit operator bool() const { return handle_ != -1; }
    bool directory() const { return directory_; }

private:
    Node(std::intptr_t handle, bool directory) : handle_(handle), directory_(directory) {}
    void close();

    std::intptr_t handle_ = -1;
    bool directory_ = false;
};

} // namespace lay::platform
