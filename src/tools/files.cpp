#include "tools/tool.hpp"
#include "platform/process.hpp"
#include "platform/win_util.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ode {

fs::path resolve(const fs::path& root, std::string_view path) {
#ifdef _WIN32
    // Git Bash spells Windows drives as /c/... while std::filesystem expects C:/....
    if (path.size() >= 3 && path[0] == '/' && path[2] == '/' &&
        ((path[1] >= 'a' && path[1] <= 'z') || (path[1] >= 'A' && path[1] <= 'Z'))) {
        std::string windows_path(path);
        windows_path[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(path[1])));
        windows_path[1] = ':';
        return fs::path(std::u8string(windows_path.begin(), windows_path.end())).lexically_normal();
    }
#endif
    fs::path p = fs::path(std::u8string(path.begin(), path.end()));
    if (p.is_relative()) p = root / p;
    return p.lexically_normal();
}

bool inside(const fs::path& root, const fs::path& path) {
    const auto rel = path.lexically_normal().lexically_relative(root.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

std::optional<write_target> parallel_write_target(const fs::path& root, const fs::path& path) {
    if (root == path || !inside(root, path)) return std::nullopt;
    const auto name = path.filename().native();
    if (name.empty() || name.back() == L'.' || name.back() == L' ' ||
        name.find_first_of(L"~:<>\"|?*") != std::wstring::npos) return std::nullopt;

    std::error_code ec;
    const auto parent = fs::canonical(path.parent_path(), ec);
    if (ec || !inside(root, parent) || !fs::is_directory(parent, ec) || ec) return std::nullopt;
    const auto target = parent / path.filename();
    const auto attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return std::nullopt;
        // Keep new names away from DOS devices and names with ambiguous Win32 normalization.
        constexpr std::wstring_view simple = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";
        if (name.find_first_not_of(simple) != std::wstring::npos) return std::nullopt;
        const auto stem = name.substr(0, name.find(L'.'));
        for (const auto* device : {L"CON", L"PRN", L"AUX", L"NUL", L"COM0", L"COM1", L"COM2", L"COM3", L"COM4",
                                   L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT0", L"LPT1", L"LPT2",
                                   L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"}) {
            if (CompareStringOrdinal(stem.c_str(), -1, device, -1, TRUE) == CSTR_EQUAL) return std::nullopt;
        }
        return write_target{target, false};
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0 ||
        fs::hard_link_count(target, ec) != 1 || ec) return std::nullopt;
    return write_target{target, true};
}

bool independent_write_targets(const write_target& left, const write_target& right) {
    std::error_code ec;
    if (left.exists && right.exists) return !fs::equivalent(left.path, right.path, ec) && !ec;
    const bool same_parent = fs::equivalent(left.path.parent_path(), right.path.parent_path(), ec);
    if (ec) return false;
    if (!same_parent) return true;
    const auto left_name = left.path.filename();
    const auto right_name = right.path.filename();
    const int comparison = CompareStringOrdinal(left_name.c_str(), -1, right_name.c_str(), -1, TRUE);
    return comparison == CSTR_LESS_THAN || comparison == CSTR_GREATER_THAN;
}

std::string display_path(const fs::path& root, const fs::path& path) {
    if (inside(root, path)) {
        const auto rel = path.lexically_normal().lexically_relative(root.lexically_normal()).generic_u8string();
        return std::string(rel.begin(), rel.end());
    }
    const auto abs = path.generic_u8string();
    return std::string(abs.begin(), abs.end());
}

std::string display_absolute_path(const fs::path& path) {
    std::error_code ec;
    const auto absolute = fs::absolute(path, ec).lexically_normal();
    const auto shown = (ec ? path : absolute).generic_u8string();
    return std::string(shown.begin(), shown.end());
}

std::optional<file_stamp> stamp(const fs::path& path) {
    std::error_code ec;
    const auto mtime = fs::last_write_time(path, ec);
    if (ec) return std::nullopt;
    const auto size = fs::file_size(path, ec);
    if (ec) return std::nullopt;
    return file_stamp{mtime, size};
}

namespace {

bool match_here(std::string_view p, std::string_view s);

bool match_braces(std::string_view p, std::string_view s) {
    const auto close = p.find('}');
    if (close == std::string_view::npos) return !s.empty() && s[0] == '{' && match_here(p.substr(1), s.substr(1));
    const auto body = p.substr(1, close - 1);
    const auto rest = p.substr(close + 1);
    std::size_t start = 0;
    while (true) {
        const auto comma = body.find(',', start);
        const auto option = body.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (match_here(std::string(option) + std::string(rest), s)) return true;
        if (comma == std::string_view::npos) return false;
        start = comma + 1;
    }
}

bool match_here(std::string_view p, std::string_view s) {
    while (!p.empty()) {
        if (p.starts_with("**")) {
            p.remove_prefix(2);
            if (p.starts_with("/")) {
                // "**/" matches zero or more whole directories.
                const auto rest = p.substr(1);
                if (match_here(rest, s)) return true;
                for (std::size_t i = 0; i < s.size(); ++i) {
                    if (s[i] == '/' && match_here(rest, s.substr(i + 1))) return true;
                }
                return false;
            }
            for (std::size_t i = 0; i <= s.size(); ++i) {
                if (match_here(p, s.substr(i))) return true;
            }
            return false;
        }
        const char c = p.front();
        if (c == '*') {
            p.remove_prefix(1);
            for (std::size_t i = 0; i <= s.size(); ++i) {
                if (match_here(p, s.substr(i))) return true;
                if (i < s.size() && s[i] == '/') return false;
            }
            return false;
        }
        if (c == '{') return match_braces(p, s);
        if (s.empty()) return false;
        if (c == '?') {
            if (s.front() == '/') return false;
        } else if (c != s.front()) {
            return false;
        }
        p.remove_prefix(1);
        s.remove_prefix(1);
    }
    return s.empty();
}

const char* skipped_dirs[] = {".git", "node_modules", "build", "out", "dist", "target", ".venv", "__pycache__", ".cache"};

std::vector<fs::path> walk(const fs::path& under) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(under, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (std::ranges::find(skipped_dirs, name) != std::end(skipped_dirs)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) files.push_back(it->path());
        if (files.size() > 200000) break;
    }
    return files;
}

} // namespace

bool glob_match(std::string_view pattern, std::string_view path) {
    return match_here(pattern, path);
}

std::vector<fs::path> workspace_files(const fs::path& root, const fs::path& under) {
    // Inside a git repository the index and .gitignore decide what belongs to the project.
    static const auto git = find_on_path("git");
    if (git && fs::exists(root / ".git")) {
        cancel_token never;
        process_request req{*git, {"-c", "core.quotepath=off", "ls-files", "-co", "--exclude-standard", "-z"}, under,
                            std::chrono::milliseconds(15000)};
        auto r = run_process(req, never);
        if (r.exit_code == 0) {
            std::vector<fs::path> files;
            std::size_t start = 0;
            while (start < r.output.size()) {
                auto end = r.output.find('\0', start);
                if (end == std::string::npos) end = r.output.size();
                const auto rel = r.output.substr(start, end - start);
                if (!rel.empty()) files.push_back((under / fs::path(std::u8string(rel.begin(), rel.end()))).lexically_normal());
                start = end + 1;
            }
            return files;
        }
    }
    return walk(under);
}

void turn_backup::remember(const fs::path& path, std::optional<std::string> original) {
    if (originals.contains(path)) return;
    // Windows file identity can be shared by distinct alternate data streams.
    if (original && path.relative_path().native().find(L':') == std::wstring::npos) {
        for (const auto& [prior, unused] : originals) {
            if (prior.relative_path().native().find(L':') != std::wstring::npos) continue;
            std::error_code ec;
            if (fs::equivalent(prior, path, ec) && !ec) return;
        }
    }
    originals.emplace(path, std::move(original));
}

restore_result restore_report(const turn_backup& backup, const fs::path& root) {
    restore_result result;
    for (const auto& [path, original] : backup.originals) {
        std::error_code ec;
        if (original) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(original->data(), static_cast<std::streamsize>(original->size()));
                out.close();
            }
            if (!out) result.failed.push_back(display_path(root, path));
            else result.restored.push_back(display_path(root, path));
        } else {
            const bool removed = fs::remove(path, ec);
            if (ec || (!removed && fs::exists(path, ec)) || ec) result.failed.push_back(display_path(root, path));
            else result.restored.push_back(display_path(root, path));
        }
    }
    return result;
}

std::vector<std::string> restore(const turn_backup& backup, const fs::path& root) {
    return restore_report(backup, root).restored;
}

} // namespace ode
