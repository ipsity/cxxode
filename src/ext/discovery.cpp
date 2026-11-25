#include "ext/discovery.hpp"
#include "base/files.hpp"
#include "base/json.hpp"

#include <algorithm>

namespace ode {

namespace fs = std::filesystem;

namespace {

std::string_view trim(std::string_view value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}

} // namespace

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
    std::string list_key;
    while (line_start < yaml.size()) {
        auto line_end = yaml.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = yaml.size();
        const auto line = trim(yaml.substr(line_start, line_end - line_start));
        if ((line.starts_with("- ") || line.starts_with("-\t")) && !list_key.empty()) {
            meta[list_key] += '\n';
            meta[list_key] += trim(line.substr(2));
        } else if (const auto colon = line.find(':'); colon != std::string_view::npos && colon > 0) {
            const auto key = std::string(trim(line.substr(0, colon)));
            auto value = trim(line.substr(colon + 1));
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
                value = value.substr(1, value.size() - 2);
            }
            meta[key] = std::string(value);
            list_key = value.empty() ? key : std::string{};
        }
        line_start = line_end + 1;
    }

    const auto body_start = end + (end_lf != std::string_view::npos ? 5 : 6);
    return {meta, std::string(text.substr(std::min(body_start, text.size())))};
}

std::vector<std::string> frontmatter_list(std::string_view value, bool split_spaces) {
    value = trim(value);
    if (value.starts_with('[') && value.ends_with(']')) value = value.substr(1, value.size() - 2);
    std::vector<std::string> result;
    const auto append = [&](std::string_view item) {
        item = trim(item);
        if (item.size() >= 2 && (item.front() == '"' || item.front() == '\'') && item.back() == item.front()) {
            item = item.substr(1, item.size() - 2);
        }
        if (!item.empty()) result.emplace_back(item);
    };
    std::size_t start = 0;
    int parentheses = 0;
    char quote = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            ++parentheses;
        } else if (c == ')' && parentheses > 0) {
            --parentheses;
        } else if (parentheses == 0 && (c == ',' || c == '\n' || (split_spaces && (c == ' ' || c == '\t')))) {
            append(value.substr(start, i - start));
            start = i + 1;
        }
    }
    append(value.substr(start));
    return result;
}

} // namespace ode
