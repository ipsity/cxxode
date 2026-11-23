#include "tools/read_only.hpp"
#include "core/utf8.hpp"
#include "tools/file_version.hpp"
#include "tools/utc.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace lay {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr std::size_t output_limit = 32 * 1024;
constexpr std::size_t file_limit = 1024 * 1024;
constexpr std::size_t search_bytes = 16 * file_limit;
constexpr std::size_t entry_limit = 4096;

template <typename Visit>
bool components(std::string_view path, const Visit& visit) {
    std::size_t begin = 0;
    std::size_t count = 0;
    while (true) {
        const auto end = path.find('/', begin);
        const auto name = path.substr(begin, end == path.npos ? path.npos : end - begin);
        if (++count > 32 || !visit(name, path.substr(0, end))) {
            return false;
        }
        if (end == path.npos) {
            return true;
        }
        begin = end + 1;
    }
}

bool safe_name(std::string_view name) {
    return !name.empty() && name.size() <= 255 * 4 && valid_utf8(name) && name != "." && name != ".." &&
        name.back() != ' ' && name.back() != '.' && name.find_first_of(":\"<>|?*\\") == name.npos &&
        std::ranges::none_of(name, [](unsigned char byte) { return byte < 32 || byte == 127; });
}

bool default_excluded(std::string_view name) {
    std::string lower(name);
    for (auto& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return lower == ".git" || lower == ".hg" || lower == ".svn" || lower == ".ssh" ||
        lower == ".aws" || lower == ".gnupg" || lower == ".npmrc" || lower == ".netrc" ||
        lower == "credentials.json" || lower == "id_rsa" || lower == "id_ed25519" ||
        lower.starts_with(".lay-code-") ||
        lower.starts_with(".env") || lower.ends_with(".pem") || lower.ends_with(".key") ||
        lower.ends_with(".p12");
}

std::string joined(std::string_view parent, std::string_view basename) {
    return parent == "." ? std::string(basename) : std::string(parent) + "/" + std::string(basename);
}

struct Text {
    std::string bytes;
    std::size_t read_bytes;
    bool truncated;
    bool supported;
    std::string version;
};

Text read_text(const platform::Node& node, std::size_t limit, bool track_version = false) {
    auto bytes = node.read(limit + 1);
    const auto read_bytes = bytes.size();
    const bool truncated = read_bytes > limit;
    auto version = track_version && !truncated ? file_version(node, bytes) : std::string{};
    if (truncated) {
        const auto newline = std::string_view(bytes).substr(0, limit).find_last_of('\n');
        bytes.resize(newline == std::string::npos ? 0 : newline + 1);
    }
    if (bytes.starts_with("\xef\xbb\xbf")) {
        bytes.erase(0, 3);
    }
    const bool supported = bytes.find('\0') == std::string::npos && valid_utf8(bytes);
    return {std::move(bytes), read_bytes, truncated, supported, std::move(version)};
}

template <typename Visit>
void for_lines(std::string_view text, const Visit& visit) {
    std::size_t line = 1;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto current = text.substr(0, end);
        if (current.ends_with('\r')) {
            current.remove_suffix(1);
        }
        if (!visit(line++, current) || end == text.npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
}

} // namespace

ReadOnlyWorkspace::ReadOnlyWorkspace(const std::filesystem::path& root, std::vector<std::string> excluded,
                                     bool editing, bool persistent)
    : root_(platform::Node::root(root)), excluded_(std::move(excluded)), editing_(editing), persistent_(persistent) {
    for (const auto& path : excluded_) {
        if (path.size() > 2048 || !components(path, [](std::string_view name, std::string_view) {
                return safe_name(name);
            })) {
            throw std::invalid_argument("exclusions must be workspace-relative paths using /");
        }
    }
}

bool ReadOnlyWorkspace::allowed(std::string_view path) const {
    return path == "." || (path.size() <= 2048 && components(path, [&](std::string_view name, std::string_view prefix) {
        return safe_name(name) && !default_excluded(name) &&
            std::ranges::none_of(excluded_, [&](const auto& entry) { return platform::path_equal(entry, prefix); });
    }));
}

platform::Node ReadOnlyWorkspace::open(std::string_view path) const {
    std::vector<platform::Node> parents;
    if (!components(path, [&](std::string_view name, std::string_view) {
            auto child = (parents.empty() ? root_ : parents.back()).child(name);
            if (!child) {
                return false;
            }
            parents.push_back(std::move(child));
            return true;
        })) {
        return {};
    }
    return std::move(parents.back());
}

bool ReadOnlyWorkspace::matches_file_version(std::string_view path, std::string_view version) const {
    if (version.empty() || path == "." || !allowed(path)) {
        return false;
    }
    try {
        auto node = open(path);
        if (!node || node.directory()) {
            return false;
        }
        const auto limit = editing_ ? editable_bytes : file_limit;
        const auto bytes = node.read(limit + 1);
        return bytes.size() <= limit && file_version(node, bytes) == version;
    } catch (const std::runtime_error&) {
        return false;
    }
}

std::string_view ReadOnlyWorkspace::authorize(const ToolCall& call) const {
    const auto mode = validate(call);
    if (!mode.empty()) {
        return mode;
    }
    return allowed(text_arg(call, "path")) ? "" : "path is excluded or not workspace-relative";
}

ToolResult ReadOnlyWorkspace::execute(const ToolCall& call, const StopRequested& stop_requested) const {
    if (const auto denial = authorize(call); !denial.empty()) {
        return {ToolStatus::failed, std::string(denial)};
    }
    const auto path = text_arg(call, "path");
    try {
        if (stop_requested()) {
            return {ToolStatus::failed, "cancelled"};
        }
        const auto dispatch = [&](const platform::Node& node) {
            return call.name == "read" ? read(node, call) : search(node, call, stop_requested);
        };
        if (path == ".") {
            return dispatch(root_);
        }
        auto node = open(path);
        if (!node) {
            return {ToolStatus::failed, "path is missing, inaccessible or an unsupported filesystem object"};
        }
        return dispatch(node);
    } catch (const std::runtime_error& error) {
        return {ToolStatus::failed, error.what()};
    }
}

ToolResult ReadOnlyWorkspace::read(const platform::Node& node, const ToolCall& call) const {
    const auto path = text_arg(call, "path");
    const auto start = number_arg(call, "start_line", 1);
    const auto limit = number_arg(call, "line_count", 120);
    std::string output = std::string(path) + (node.directory() ? " (directory)\n" : "\n");
    std::size_t next = 0;
    std::string version;
    std::string observed;
    auto add = [&](std::size_t line, std::string_view text) {
        if (line < start) {
            return true;
        }
        if (line - start == limit) {
            next = line;
            return false;
        }
        const auto label = std::to_string(line) + ": ";
        if (text.size() + output.size() + label.size() + 256 > output_limit) {
            next = line;
            if (line == start) {
                output += label + "[line exceeds the 32768-byte tool result limit]\n";
                ++next;
            }
            return false;
        }
        output += label;
        output += text;
        output += '\n';
        return true;
    };
    if (node.directory()) {
        auto entries = node.entries(entry_limit);
        std::ranges::sort(entries.items, {}, &platform::Entry::name);
        std::size_t line = 1;
        for (const auto& entry : entries.items) {
            const auto full = joined(path, entry.name);
            if (entry.supported && allowed(full) &&
                !add(line++, entry.name + (entry.directory ? "/" : ""))) {
                break;
            }
        }
        if (entries.truncated) {
            output += "[directory enumeration limited to 4096 entries]\n";
        }
        if (line == 1) {
            output += "[no visible entries]\n";
        }
    } else {
        const auto bytes = editing_ ? editable_bytes : file_limit;
        auto text = read_text(node, bytes, editing_ || persistent_);
        if (!text.supported) {
            return {ToolStatus::failed, "file is binary or its encoding is not UTF-8"};
        }
        for_lines(text.bytes, add);
        version = std::move(text.version);
        observed = persistent_ ? utc_now() : std::string{};
        if (text.truncated) {
            output += "[file scan limited to " + std::to_string(bytes) +
                " bytes; partial lines omitted]\n";
        } else if (text.bytes.empty()) {
            output += "[empty file]\n";
        }
    }
    if (next) {
        output += "next_line=" + std::to_string(next) + '\n';
    }
    if (editing_ && !version.empty()) {
        output += "version=" + version + '\n';
    }
    return {ToolStatus::ok, std::move(output), std::move(version), std::move(observed)};
}

ToolResult ReadOnlyWorkspace::search(const platform::Node& node, const ToolCall& call,
                                     const StopRequested& stop_requested) const {
    if (!node.directory()) {
        return {ToolStatus::failed, "search path must be a directory"};
    }
    const auto path = text_arg(call, "path");
    const auto query = text_arg(call, "query");
    const auto suffix = text_arg(call, "file_suffix");
    const auto limit = number_arg(call, "max_results", 30);
    const auto deadline = Clock::now() + 2s;
    std::size_t remaining_entries = entry_limit;
    std::size_t remaining_bytes = search_bytes;
    std::size_t matches = 0;
    std::size_t skipped = 0;
    bool limited = false;
    bool stop_scan = false;
    std::string output;

    const auto visit = [&](const auto& self, const platform::Node& directory,
                           std::string_view parent, std::size_t depth) -> void {
        if (depth >= 32 || remaining_entries == 0 || stop_scan ||
            stop_requested() || Clock::now() >= deadline) {
            limited = true;
            return;
        }
        auto entries = directory.entries(remaining_entries);
        remaining_entries -= entries.items.size();
        std::ranges::sort(entries.items, {}, &platform::Entry::name);
        limited = limited || entries.truncated;
        for (const auto& entry : entries.items) {
            if (stop_scan || remaining_bytes == 0 ||
                stop_requested() || Clock::now() >= deadline) {
                stop_scan = limited = true;
                return;
            }
            const auto full = joined(parent, entry.name);
            if (!entry.supported || !allowed(full)) {
                ++skipped;
                continue;
            }
            try {
                const auto child = directory.child(entry.name);
                if (!child) {
                    ++skipped;
                } else if (child.directory()) {
                    self(self, child, full, depth + 1);
                } else if (entry.name.ends_with(suffix)) {
                    const auto text = read_text(child, std::min(file_limit, remaining_bytes - 1));
                    remaining_bytes -= text.read_bytes;
                    limited = limited || text.truncated;
                    if (!text.supported) {
                        ++skipped;
                        continue;
                    }
                    for_lines(text.bytes, [&](std::size_t number, std::string_view line) {
                        if (stop_requested() || Clock::now() >= deadline) {
                            stop_scan = limited = true;
                            return false;
                        }
                        auto position = line.find(query);
                        if (position == line.npos) {
                            return true;
                        }
                        if (matches == limit || output.size() + full.size() + 600 > output_limit) {
                            stop_scan = limited = true;
                            return false;
                        }
                        output += full + ":" + std::to_string(number) + ": ";
                        if (position > 80) {
                            position -= 80;
                            while (continuation(static_cast<unsigned char>(line[position]))) {
                                ++position;
                            }
                            line.remove_prefix(position);
                            output += "...";
                        }
                        output += utf8_prefix(line, 384);
                        if (line.size() > 384) {
                            output += "...";
                        }
                        output += '\n';
                        ++matches;
                        return true;
                    });
                }
            } catch (const std::runtime_error&) {
                ++skipped;
            }
        }
    };
    visit(visit, node, path, 0);
    if (stop_requested()) {
        return {ToolStatus::failed, "cancelled"};
    }
    output += "matches=" + std::to_string(matches) + " skipped=" + std::to_string(skipped) +
        " scanned_bytes=" + std::to_string(search_bytes - remaining_bytes) + '\n';
    if (limited) {
        output += "[partial coverage: result, scan, depth or time limit reached]\n";
    }
    return {ToolStatus::ok, std::move(output)};
}

} // namespace lay
