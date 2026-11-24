#include "tools/tool.hpp"

#include <algorithm>
#include <regex>

namespace lay {
namespace {

constexpr std::size_t max_scan_bytes = 2 * 1024 * 1024;

fs::path base_path(const json& input, const tool_context& ctx) {
    const auto p = input.value("path", std::string{});
    return p.empty() ? ctx.root : resolve(ctx.root, p);
}

std::string relative_to(const fs::path& base, const fs::path& path) {
    const auto rel = path.lexically_relative(base).generic_u8string();
    return std::string(rel.begin(), rel.end());
}

// A pattern without a slash matches the file name at any depth, as in ripgrep and .gitignore.
bool filter_match(std::string_view pattern, const std::string& rel) {
    if (pattern.find('/') != std::string_view::npos) return glob_match(pattern, rel);
    const auto slash = rel.rfind('/');
    return glob_match(pattern, slash == std::string::npos ? rel : rel.substr(slash + 1));
}

tool_output glob_tool(const json& input, tool_context& ctx) {
    const auto pattern = input.value("pattern", std::string{});
    if (pattern.empty()) return {"pattern is required.", true, "失败", {}};
    const auto base = base_path(input, ctx);
    std::vector<std::pair<fs::file_time_type, std::string>> hits;
    for (const auto& file : workspace_files(ctx.root, base)) {
        const auto rel = relative_to(base, file);
        if (!filter_match(pattern, rel)) continue;
        std::error_code ec;
        hits.emplace_back(fs::last_write_time(file, ec), display_path(ctx.root, file));
    }
    std::ranges::sort(hits, std::greater{}, &std::pair<fs::file_time_type, std::string>::first);
    std::string out;
    const auto shown = std::min<std::size_t>(hits.size(), 200);
    for (std::size_t i = 0; i < shown; ++i) out += hits[i].second + "\n";
    if (hits.empty()) out = "No files matched.\n";
    else if (hits.size() > shown) out += "… " + std::to_string(hits.size() - shown) + " more; narrow the pattern.\n";
    return {out, false, std::to_string(hits.size()) + " 个文件", {}};
}

tool_output grep_tool(const json& input, tool_context& ctx) {
    const auto pattern = input.value("pattern", std::string{});
    if (pattern.empty()) return {"pattern is required.", true, "失败", {}};
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    if (input.value("-i", false)) flags |= std::regex::icase;
    std::regex re;
    try {
        re = std::regex(pattern, flags);
    } catch (const std::regex_error& e) {
        return {std::string("Invalid regular expression: ") + e.what(), true, "失败", {}};
    }
    const auto mode = input.value("output_mode", std::string("files_with_matches"));
    const auto filter = input.value("glob", std::string{});
    const auto limit = static_cast<std::size_t>(std::max<std::int64_t>(1, input.value("head_limit", std::int64_t{250})));
    const auto base = base_path(input, ctx);
    std::error_code ec;
    const auto files = fs::is_regular_file(base, ec) ? std::vector<fs::path>{base} : workspace_files(ctx.root, base);

    std::string out;
    std::size_t entries = 0, matched_files = 0;
    bool truncated = false;
    for (const auto& file : files) {
        if (ctx.cancel && ctx.cancel->cancelled()) break;
        if (!filter.empty() && !filter_match(filter, relative_to(base, file))) continue;
        if (fs::file_size(file, ec) > max_scan_bytes || ec) continue;
        const auto content = read_file(file);
        if (!content || content->find('\0', 0) < std::min<std::size_t>(content->size(), 8192)) continue;
        std::size_t count = 0, line_no = 0, start = 0;
        const auto shown = display_path(ctx.root, file);
        while (start <= content->size()) {
            auto end = content->find('\n', start);
            if (end == std::string::npos) end = content->size();
            ++line_no;
            auto line = std::string_view(*content).substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (std::regex_search(line.begin(), line.end(), re)) {
                ++count;
                if (mode == "content") {
                    if (entries >= limit) { truncated = true; break; }
                    out += shown + ":" + std::to_string(line_no) + ":" + std::string(line.substr(0, 500)) + "\n";
                    ++entries;
                } else if (mode == "files_with_matches") {
                    break;
                }
            }
            if (end == content->size()) break;
            start = end + 1;
        }
        if (!count) continue;
        ++matched_files;
        if (mode == "files_with_matches") {
            if (entries >= limit) { truncated = true; break; }
            out += shown + "\n";
            ++entries;
        } else if (mode == "count") {
            if (entries >= limit) { truncated = true; break; }
            out += shown + ":" + std::to_string(count) + "\n";
            ++entries;
        }
        if (truncated) break;
    }
    if (out.empty()) out = "No matches.\n";
    if (truncated) out += "… results truncated at head_limit=" + std::to_string(limit) + ".\n";
    return {out, false, std::to_string(matched_files) + " 个文件", {}};
}

} // namespace

std::vector<tool> search_tools() {
    const auto label = [](const json& input) {
        auto text = input.value("pattern", std::string{});
        const auto path = input.value("path", std::string{});
        if (!path.empty()) text += "  in " + path;
        return text;
    };
    return {
        {"Grep", "Search file contents with an ECMAScript regular expression. Respects .gitignore in git repositories. "
                 "output_mode: files_with_matches (default), content (path:line:text), or count.",
         {{"type", "object"},
          {"properties",
           {{"pattern", {{"type", "string"}}},
            {"path", {{"type", "string"}, {"description", "File or directory; defaults to the workspace"}}},
            {"glob", {{"type", "string"}, {"description", "File filter such as *.cpp or src/**/*.hpp"}}},
            {"output_mode", {{"type", "string"}, {"enum", {"content", "files_with_matches", "count"}}}},
            {"-i", {{"type", "boolean"}}},
            {"head_limit", {{"type", "integer"}}}}},
          {"required", {"pattern"}}},
         tool_access::read, grep_tool, label},
        {"Glob", "Find files by glob pattern such as **/*.cpp, newest first. Respects .gitignore in git repositories.",
         {{"type", "object"},
          {"properties", {{"pattern", {{"type", "string"}}}, {"path", {{"type", "string"}}}}},
          {"required", {"pattern"}}},
         tool_access::read, glob_tool, label},
    };
}

} // namespace lay
