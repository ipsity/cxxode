#include "ext/plugins.hpp"
#include "core/session.hpp"
#include "tools/tool.hpp"

namespace ode {

namespace fs = std::filesystem;

std::vector<plugin_info> load_plugins() {
    std::vector<plugin_info> plugins;
    const auto home = ode_home().parent_path();

    std::vector<fs::path> plugin_dirs = {
        home / ".ode" / "plugins",
        home / ".claude" / "plugins"
    };

    for (const auto& plugins_dir : plugin_dirs) {
        std::error_code ec;
        if (!fs::is_directory(plugins_dir, ec)) continue;

        for (const auto& entry : fs::directory_iterator(plugins_dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            const auto plugin_path = entry.path();
            const auto meta_file = plugin_path / ".claude-plugin" / "plugin.json";

            if (!fs::is_regular_file(meta_file, ec)) continue;

            const auto content = read_file(meta_file);
            if (!content) continue;

            auto meta = json::parse(*content, nullptr, false, true);
            if (meta.is_discarded() || !meta.is_object()) continue;

            plugin_info info;
            info.name = meta.value("name", plugin_path.filename().string());
            info.version = meta.value("version", "");
            info.description = meta.value("description", "");
            info.author = meta.value("author", "");
            info.path = plugin_path;

            plugins.push_back(info);
        }
    }

    return plugins;
}

std::vector<std::filesystem::path> plugin_search_paths() {
    std::vector<fs::path> paths;

    for (const auto& plugin : load_plugins()) {
        paths.push_back(plugin.path);
    }

    return paths;
}

} // namespace ode
