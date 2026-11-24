#include "tools/tool.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ode {

fs::path resolve(const fs::path& root, std::string_view path) {
    fs::path p = fs::path(std::u8string(path.begin(), path.end()));
    if (p.is_relative()) p = root / p;
    return p.lexically_normal();
}

bool inside(const fs::path& root, const fs::path& path) {
    const auto rel = path.lexically_normal().lexically_relative(root.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

std::string display_path(const fs::path& root, const fs::path& path) {
    if (inside(root, path)) {
        const auto rel = path.lexically_normal().lexically_relative(root.lexically_normal()).generic_u8string();
        return std::string(rel.begin(), rel.end());
    }
    const auto abs = path.generic_u8string();
    return std::string(abs.begin(), abs.end());
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream out;
    out << in.rdbuf();
    return std::move(out).str();
}

std::optional<file_stamp> stamp(const fs::path& path) {
    std::error_code ec;
    const auto mtime = fs::last_write_time(path, ec);
    if (ec) return std::nullopt;
    const auto size = fs::file_size(path, ec);
    if (ec) return std::nullopt;
    return file_stamp{mtime, size};
}

std::string clip(std::string text, std::size_t limit) {
    if (text.size() <= limit) return text;
    const auto half = limit / 2;
    auto head = half, tail = text.size() - half;
    // Never cut inside a UTF-8 sequence.
    while (head > 0 && (static_cast<unsigned char>(text[head]) & 0xC0) == 0x80) --head;
    while (tail < text.size() && (static_cast<unsigned char>(text[tail]) & 0xC0) == 0x80) ++tail;
    return text.substr(0, head) + "\n… [" + std::to_string(tail - head) + " bytes omitted] …\n" + text.substr(tail);
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

std::vector<std::string> restore(const turn_backup& backup, const fs::path& root) {
    std::vector<std::string> restored;
    for (const auto& [path, original] : backup.originals) {
        std::error_code ec;
        if (original) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << *original;
        } else {
            fs::remove(path, ec);
        }
        restored.push_back(display_path(root, path));
    }
    return restored;
}

} // namespace ode
