#include "app/app.hpp"
#include "ui/render.hpp"

#include <cstdio>

namespace ode {

std::string short_count(std::size_t n) {
    char buf[32];
    if (n < 1000) std::snprintf(buf, sizeof(buf), "%zu", n);
    else if (n < 100000) std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(n) / 1000.0);
    else if (n < 1000000) std::snprintf(buf, sizeof(buf), "%zuk", n / 1000);
    else std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1000000.0);
    return buf;
}

std::string git_branch(const fs::path& root) {
    for (auto d = root;; d = d.parent_path()) {
        const auto head = read_file(d / ".git" / "HEAD");
        if (head) {
            constexpr std::string_view ref = "ref: refs/heads/";
            auto text = *head;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            if (text.starts_with(ref)) return text.substr(ref.size());
            return text.substr(0, 7);
        }
        if (d == d.parent_path()) return {};
    }
}

std::string home_relative(const fs::path& path) {
    const auto home = ode_home().parent_path();
    std::string shown;
    if (inside(home, path)) {
        const auto rel = path.lexically_relative(home).generic_u8string();
        shown = "~/" + std::string(rel.begin(), rel.end());
    } else {
        const auto abs = path.generic_u8string();
        shown.assign(abs.begin(), abs.end());
    }
    return shown;
}

std::string tool_label(const tool& t, const json& input, const fs::path& root) {
    if (t.name == "Read" || t.name == "Write" || t.name == "Edit") {
        const auto p = input.value("file_path", std::string{});
        auto shown = display_path(root, resolve(root, p));
        if (t.name == "Read" && input.contains("offset")) shown += "  from line " + std::to_string(input.value("offset", 1));
        return shown;
    }
    if (t.label) return ui::one_line(t.label(input));
    return t.name;
}

} // namespace ode
