#include "skills/skill.hpp"
#include "core/session.hpp"

#include <algorithm>

namespace lay {

std::string skills_system_prompt(const std::vector<skill>& skills) {
    if (skills.empty()) return {};
    std::string prompt = "\n# Available Skills\n\nYou can invoke pre-defined skills with the `skill` tool:\n\n";
    for (const auto& s : skills) {
        prompt += "- **" + s.name + "**";
        if (!s.description.empty()) prompt += ": " + s.description;
        prompt += "\n";
    }
    prompt += "\nTo use a skill, call `skill` with its name. The skill's instructions will be loaded for you to follow.\n";
    return prompt;
}

std::string skill::load_body() const {
    if (!body.empty()) return body;
    return read_file(path).value_or("");
}

std::pair<std::map<std::string, std::string>, std::string> parse_frontmatter(std::string_view text) {
    std::map<std::string, std::string> meta;
    // YAML frontmatter starts with --- and ends with ---
    if (!text.starts_with("---\n") && !text.starts_with("---\r\n")) return {meta, std::string(text)};

    const auto start = text.find('\n') + 1;
    const auto end_marker = text.find("\n---\n", start);
    const auto end_marker_crlf = text.find("\n---\r\n", start);
    const auto end = end_marker != std::string_view::npos ? end_marker : end_marker_crlf;

    if (end == std::string_view::npos) return {meta, std::string(text)};

    const auto yaml = text.substr(start, end - start);
    std::size_t line_start = 0;
    while (line_start < yaml.size()) {
        auto line_end = yaml.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = yaml.size();
        auto line = yaml.substr(line_start, line_end - line_start);
        // Trim \r if present
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const auto colon = line.find(':');
        if (colon != std::string_view::npos && colon > 0) {
            auto key = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            // Trim leading space from value
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            meta[std::string(key)] = std::string(value);
        }
        line_start = line_end + 1;
    }

    // Body starts after the closing ---
    const auto body_start = end + (end_marker != std::string_view::npos ? 5 : 6);  // \n---\n or \n---\r\n
    return {meta, std::string(text.substr(std::min(body_start, text.size())))};
}

std::vector<skill> load_skills(const fs::path& workspace) {
    std::vector<skill> skills;
    std::vector<fs::path> search_paths;

    // User-level skills
    const auto home = lay_home().parent_path();
    search_paths.push_back(home / ".claude" / "skills");

    // Workspace skills - walk up to git root
    for (auto dir = workspace; ; dir = dir.parent_path()) {
        search_paths.push_back(dir / ".claude" / "skills");
        if (fs::exists(dir / ".git") || dir == dir.parent_path()) break;
    }

    for (const auto& dir : search_paths) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;

        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto& path = entry.path();
            if (path.extension() != ".md") continue;

            const auto content = read_file(path);
            if (!content) continue;

            auto [meta, body] = parse_frontmatter(*content);
            const auto name = meta.count("name") ? meta["name"] : path.stem().string();
            const auto desc = meta.count("description") ? meta["description"] : "";

            // Avoid duplicates - workspace skills override user skills
            bool found = false;
            for (auto& s : skills) {
                if (s.name == name) {
                    s.path = path;
                    s.description = desc;
                    s.body.clear();
                    found = true;
                    break;
                }
            }
            if (!found) skills.push_back({name, desc, path, body});
        }
    }

    std::ranges::sort(skills, {}, &skill::name);
    return skills;
}

} // namespace lay
