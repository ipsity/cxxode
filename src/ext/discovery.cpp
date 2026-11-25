#include "ext/discovery.hpp"
#include "base/files.hpp"
#include "base/json.hpp"

#include <algorithm>

namespace ode {

namespace fs = std::filesystem;

std::vector<plugin_info> load_plugins() {
    std::vector<plugin_info> plugins;
    const auto home = user_home();
    for (const auto& root : {home / ".ode" / "plugins", home / ".claude" / "plugins"}) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory(ec)) continue;
            const auto dir = entry.path();
            const auto content = read_file(dir / ".claude-plugin" / "plugin.json");
            if (!content) continue;
            const auto meta = json::parse(*content, nullptr, false, true);
            if (meta.is_discarded() || !meta.is_object()) continue;
            plugin_info info;
            info.name = meta.value("name", dir.filename().string());
            info.version = meta.value("version", "");
            info.description = meta.value("description", "");
            // Claude Code allows author as a string or as {"name": ...}.
            if (const auto a = meta.find("author"); a != meta.end()) {
                if (a->is_string()) info.author = *a;
                else if (a->is_object()) info.author = a->value("name", "");
            }
            info.path = dir;
            plugins.push_back(std::move(info));
        }
    }
    return plugins;
}

std::vector<fs::path> extension_dirs(const fs::path& workspace, std::string_view kind) {
    const fs::path sub{std::string(kind)};
    std::vector<fs::path> dirs;
    const auto home = user_home();
    dirs.push_back(home / ".claude" / sub);
    dirs.push_back(home / ".ode" / sub);
    for (const auto& p : load_plugins()) {
        dirs.push_back(p.path / sub);
        dirs.push_back(p.path / ".claude" / sub);
    }
    for (auto dir = workspace;; dir = dir.parent_path()) {
        dirs.push_back(dir / ".claude" / sub);
        std::error_code ec;
        if (fs::exists(dir / ".git", ec) || dir == dir.parent_path()) break;
    }
    return dirs;
}

std::pair<std::map<std::string, std::string>, std::string> parse_frontmatter(std::string_view text) {
    std::map<std::string, std::string> meta;
    if (!text.starts_with("---\n") && !text.starts_with("---\r\n")) return {meta, std::string(text)};

    const auto start = text.find('\n') + 1;
    const auto end_lf = text.find("\n---\n", start);
    const auto end_crlf = text.find("\n---\r\n", start);
    const auto end = end_lf != std::string_view::npos ? end_lf : end_crlf;
    if (end == std::string_view::npos) return {meta, std::string(text)};

    const auto yaml = text.substr(start, end - start);
    std::size_t line_start = 0;
    while (line_start < yaml.size()) {
        auto line_end = yaml.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = yaml.size();
        auto line = yaml.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (const auto colon = line.find(':'); colon != std::string_view::npos && colon > 0) {
            auto value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            meta[std::string(line.substr(0, colon))] = std::string(value);
        }
        line_start = line_end + 1;
    }

    const auto body_start = end + (end_lf != std::string_view::npos ? 5 : 6);
    return {meta, std::string(text.substr(std::min(body_start, text.size())))};
}

} // namespace ode
