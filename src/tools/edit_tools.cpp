#include "tools/tool.hpp"

#include <algorithm>
#include <fstream>

namespace ode {
namespace {

constexpr std::size_t max_read_bytes = 10 * 1024 * 1024;
constexpr std::size_t max_line_chars = 2000;

std::string path_arg(const json& input) {
    return input.contains("file_path") && input["file_path"].is_string() ? input["file_path"].get<std::string>() : "";
}

tool_output fail(std::string text) { return {std::move(text), true, "failed", {}}; }

void remember(tool_context& ctx, const fs::path& path) {
    if (!ctx.seen) return;
    if (auto s = stamp(path)) (*ctx.seen)[path] = *s;
}

void back_up(tool_context& ctx, const fs::path& path, std::optional<std::string> original) {
    if (ctx.backup) ctx.backup->remember(path, std::move(original));
}

bool write_all(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return static_cast<bool>(out);
}

std::size_t count_lines(std::string_view text) {
    if (text.empty()) return 0;
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + (text.back() == '\n' ? 0 : 1);
}

std::string preview(std::string_view removed, std::string_view added) {
    std::string out;
    const auto emit = [&](std::string_view text, char sign) {
        constexpr int max_preview_lines = 8;
        int shown = 0;
        while (!text.empty() && shown < max_preview_lines) {
            const auto end = text.find('\n');
            auto line = text.substr(0, end);
            if (line.ends_with('\r')) line.remove_suffix(1);
            auto length = std::min(line.size(), max_line_chars);
            if (length < line.size()) {
                while (length > 0 && (static_cast<unsigned char>(line[length]) & 0xc0) == 0x80) --length;
            }
            out += sign;
            out += ' ';
            out.append(line.substr(0, length));
            if (length < line.size()) out += "…";
            out += '\n';
            text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
            ++shown;
        }
        if (!text.empty()) {
            out += "… ";
            out += sign;
            out += std::to_string(count_lines(text)) + " more lines\n";
        }
    };
    emit(removed, '-');
    emit(added, '+');
    return out;
}

std::string rewrite_preview(std::string_view before, std::string_view after) {
    if (before.find('\0') != std::string_view::npos || after.find('\0') != std::string_view::npos)
        return "Binary file; diff not shown.\n";
    while (!before.empty() && !after.empty()) {
        const auto line = [](std::string_view text) {
            const auto end = text.find('\n');
            return end == std::string_view::npos ? text : text.substr(0, end + 1);
        };
        const auto old_line = line(before);
        if (old_line != line(after)) break;
        before.remove_prefix(old_line.size());
        after.remove_prefix(old_line.size());
    }
    while (!before.empty() && !after.empty()) {
        const auto line = [](std::string_view text) {
            if (text.size() <= 1) return text;
            const auto start = text.rfind('\n', text.size() - 2);
            return start == std::string_view::npos ? text : text.substr(start + 1);
        };
        const auto old_line = line(before);
        if (old_line != line(after)) break;
        before.remove_suffix(old_line.size());
        after.remove_suffix(old_line.size());
    }
    return preview(before, after);
}

std::string replace_newlines(std::string_view text, bool crlf) {
    if (!crlf) return std::string(text);
    std::string out;
    for (const char c : text) {
        if (c == '\n') out += "\r\n";
        else out += c;
    }
    return out;
}

tool_output read_tool(const json& input, tool_context& ctx) {
    const auto path = resolve(ctx.root, path_arg(input));
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        std::string out;
        std::size_t n = 0;
        for (const auto& entry : fs::directory_iterator(path, ec)) {
            const auto name = entry.path().filename().u8string();
            out.append(name.begin(), name.end());
            if (entry.is_directory(ec)) out += '/';
            out += '\n';
            if (++n >= 1000) break;
        }
        return {out, false, std::to_string(n) + " items", {}};
    }
    if (!fs::is_regular_file(path, ec)) return fail("File does not exist: " + display_path(ctx.root, path));
    if (fs::file_size(path, ec) > max_read_bytes) return fail("File is larger than 10 MB. Use Grep or Bash to inspect it.");
    auto content = read_file(path);
    if (!content) return fail("Could not read " + display_path(ctx.root, path));
    if (content->find('\0') != std::string::npos) return fail("Binary file; not shown.");
    remember(ctx, path);

    const auto offset = std::max<std::int64_t>(1, input.value("offset", std::int64_t{1}));
    const auto limit = std::max<std::int64_t>(1, input.value("limit", std::int64_t{2000}));
    std::string out;
    std::int64_t line = 1, emitted = 0;
    std::size_t start = 0;
    while (start < content->size() && emitted < limit) {
        auto end = content->find('\n', start);
        if (end == std::string::npos) end = content->size();
        if (line >= offset) {
            auto text = std::string_view(*content).substr(start, end - start);
            if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
            const auto number = std::to_string(line);
            out.append(number.size() < 6 ? 6 - number.size() : 0, ' ');
            out += number;
            out += '\t';
            out.append(text.substr(0, max_line_chars));
            if (text.size() > max_line_chars) out += "…";
            out += '\n';
            ++emitted;
        }
        ++line;
        start = end + 1;
    }
    const auto total = count_lines(*content);
    if (content->empty()) out = "(empty file)\n";
    else if (static_cast<std::size_t>(offset - 1 + emitted) < total)
        out += "… file has " + std::to_string(total) + " lines; continue with offset.\n";
    return {out, false, std::to_string(emitted) + " lines", {}};
}

tool_output write_tool(const json& input, tool_context& ctx) {
    const auto path = resolve(ctx.root, path_arg(input));
    if (!input.contains("content") || !input["content"].is_string()) return fail("content is required.");
    const auto content = input["content"].get<std::string>();
    const bool existed = fs::exists(path);
    auto original = existed ? read_file(path) : std::nullopt;
    auto detail = rewrite_preview(original ? std::string_view(*original) : std::string_view{}, content);
    if (existed && !original) detail.insert(0, "Original content could not be read.\n");
    back_up(ctx, path, std::move(original));
    if (!write_all(path, content)) return fail("Could not write " + display_path(ctx.root, path));
    remember(ctx, path);
    const auto lines = count_lines(content);
    return {(existed ? "Overwrote " : "Created ") + display_path(ctx.root, path) + " (" + std::to_string(lines) + " lines).",
            false, (existed ? "overwrote " : "created ") + std::to_string(lines) + " lines", std::move(detail)};
}

tool_output edit_tool(const json& input, tool_context& ctx) {
    const auto path = resolve(ctx.root, path_arg(input));
    const auto old_text = input.value("old_string", std::string{});
    const auto new_text = input.value("new_string", std::string{});
    const bool all = input.value("replace_all", false);
    if (old_text.empty()) return fail("old_string must not be empty. Use Write to create a file.");
    if (old_text == new_text) return fail("old_string and new_string are identical.");
    auto content = read_file(path);
    if (!content) return fail("File does not exist: " + display_path(ctx.root, path));
    if (ctx.seen) {
        const auto known = ctx.seen->find(path);
        const auto now = stamp(path);
        if (known != ctx.seen->end() && now && !(known->second == *now))
            return fail("The file changed on disk since you last read it. Read it again before editing.");
    }
    const bool crlf = content->find("\r\n") != std::string::npos;
    const auto needle = replace_newlines(old_text, crlf);
    const auto replacement = replace_newlines(new_text, crlf);

    std::size_t count = 0;
    for (auto at = content->find(needle); at != std::string::npos; at = content->find(needle, at + needle.size())) ++count;
    if (count == 0) return fail("old_string was not found. Read the file and copy the exact text, including indentation.");
    if (count > 1 && !all)
        return fail("old_string occurs " + std::to_string(count) + " times. Add surrounding lines to make it unique, or set replace_all.");

    std::string updated;
    std::size_t from = 0;
    for (auto at = content->find(needle); at != std::string::npos; at = content->find(needle, from)) {
        updated.append(*content, from, at - from);
        updated += replacement;
        from = at + needle.size();
        if (!all) break;
    }
    updated.append(*content, from);
    back_up(ctx, path, std::move(content));
    if (!write_all(path, updated)) return fail("Could not write " + display_path(ctx.root, path));
    remember(ctx, path);

    const auto removed = count_lines(old_text) * count, added = count_lines(new_text) * count;
    return {"Edited " + display_path(ctx.root, path) + (count > 1 ? " (" + std::to_string(count) + " replacements)." : "."),
            false, "+" + std::to_string(added) + " -" + std::to_string(removed), preview(old_text, new_text)};
}

json schema(json properties, std::vector<std::string> required) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
            {"additionalProperties", false}};
}

} // namespace

std::vector<tool> edit_tools() {
    const auto label = [](const json& input) {
        auto p = path_arg(input);
        return p.empty() ? std::string("?") : p;
    };
    return {
        {"Read", "Read a file with line numbers (cat -n format), or list a directory. Paths are absolute or relative to "
                 "the workspace. Reads up to 2000 lines by default; use offset and limit for large files.",
         schema({{"file_path", {{"type", "string"}, {"description", "File or directory path"}}},
                 {"offset", {{"type", "integer"}, {"description", "1-based line to start from"}}},
                 {"limit", {{"type", "integer"}, {"description", "Number of lines to read"}}}},
                {"file_path"}),
         tool_access::read, read_tool, label},
        {"Write", "Create a file or overwrite it entirely. Prefer Edit for changes to existing files.",
         schema({{"file_path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}, {"file_path", "content"}),
         tool_access::write, write_tool, label, true},
        {"Edit", "Replace exact text in a file. old_string must match the file exactly (without the line-number prefix "
                 "from Read) and be unique unless replace_all is true.",
         schema({{"file_path", {{"type", "string"}}},
                 {"old_string", {{"type", "string"}}},
                 {"new_string", {{"type", "string"}}},
                 {"replace_all", {{"type", "boolean"}, {"default", false}}}},
                {"file_path", "old_string", "new_string"}),
         tool_access::write, edit_tool, label, true},
    };
}

} // namespace ode
