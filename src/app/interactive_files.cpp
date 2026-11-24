#include "app/interactive.hpp"
#include "ui/text.hpp"

#include <algorithm>
#include <filesystem>

namespace lay {

std::vector<std::string> interactive_ui::file_items() {
    const auto mention = editor_.at_mention();
    if (!mention) return {};

    const auto& pattern = *mention;
    std::vector<std::string> matches;

    // Search recursively from workspace root
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(app_.root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const auto rel = entry.path().lexically_relative(app_.root);
        const auto path_str = rel.generic_u8string();
        std::string path(path_str.begin(), path_str.end());

        // Skip common ignore patterns
        if (path.find(".git/") == 0 || path.find("node_modules/") == 0 ||
            path.find(".lay/") == 0 || path.find(".claude/") == 0) continue;

        // Match if pattern is a substring of the path
        if (pattern.empty() || path.find(pattern) != std::string::npos) {
            matches.push_back(path);
            if (matches.size() >= 50) break;  // Limit results
        }
    }

    std::ranges::sort(matches);
    return matches;
}

} // namespace lay
