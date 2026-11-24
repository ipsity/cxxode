#include "commands/command.hpp"
#include "core/session.hpp"
#include "ext/plugins.hpp"
#include "skills/skill.hpp"

#include <algorithm>

namespace ode {

std::string user_command::load_body() const {
    if (!body.empty()) return body;
    return read_file(path).value_or("");
}

std::vector<user_command> load_commands(const fs::path& workspace) {
    std::vector<user_command> commands;
    std::vector<fs::path> search_paths;

    // User-level commands
    const auto home = ode_home().parent_path();
    search_paths.push_back(home / ".claude" / "commands");
    search_paths.push_back(home / ".ode" / "commands");

    // Plugin commands
    for (const auto& plugin_path : plugin_search_paths()) {
        search_paths.push_back(plugin_path / "commands");
        search_paths.push_back(plugin_path / ".claude" / "commands");
    }

    // Workspace commands - walk up to git root
    for (auto dir = workspace; ; dir = dir.parent_path()) {
        search_paths.push_back(dir / ".claude" / "commands");
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

            // Avoid duplicates - workspace commands override user commands
            bool found = false;
            for (auto& c : commands) {
                if (c.name == name) {
                    c.path = path;
                    c.description = desc;
                    c.body.clear();
                    found = true;
                    break;
                }
            }
            if (!found) commands.push_back({name, desc, path, body});
        }
    }

    std::ranges::sort(commands, {}, &user_command::name);
    return commands;
}

std::string expand_variables(std::string_view body, const std::map<std::string, std::string>& vars) {
    std::string result;
    result.reserve(body.size());

    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '$') {
            // Find the end of the variable name (alphanumeric + underscore)
            std::size_t j = i + 1;
            while (j < body.size() && (std::isalnum(static_cast<unsigned char>(body[j])) || body[j] == '_')) ++j;

            if (j > i + 1) {
                const auto var_name = body.substr(i + 1, j - i - 1);
                const auto it = vars.find(std::string(var_name));
                if (it != vars.end()) {
                    result += it->second;
                    i = j - 1;  // -1 because the loop will increment
                    continue;
                }
            }
        }
        result += body[i];
    }

    return result;
}

} // namespace ode
