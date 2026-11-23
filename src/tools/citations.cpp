#include "tools/citations.hpp"

#include <algorithm>
#include <charconv>
#include <initializer_list>

namespace lay {
namespace {

using namespace std::string_view_literals;

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == value.npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

std::string_view field(std::string_view row, std::initializer_list<std::string_view> labels) {
    row = trim(row);
    if (row.size() >= 2 && "-*+"sv.find(row.front()) != row.npos && " \t"sv.find(row[1]) != row.npos) {
        row = trim(row.substr(2));
    }
    const auto divider = std::min(row.find(':'), row.find("："));
    if (divider == row.npos) {
        return {};
    }
    auto label = row.substr(0, divider);
    auto value = row.substr(divider + (row[divider] == ':' ? 1 : "："sv.size()));
    if (label.starts_with("**")) {
        label.remove_prefix(2);
        if (label.ends_with("**")) {
            label.remove_suffix(2);
        } else if (value.starts_with("**")) {
            value.remove_prefix(2);
        }
    }
    if (!std::ranges::any_of(labels, [&](std::string_view expected) {
        return std::ranges::equal(label, expected, [](unsigned char a, unsigned char b) {
            return (a >= 'A' && a <= 'Z' ? a + 'a' - 'A' : a) == b;
        });
    })) {
        return {};
    }
    value = trim(value);
    if (value.size() >= 2 && value.starts_with('`') && value.ends_with('`')) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::string_view take_line(std::string_view& text) {
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text.remove_prefix(end == text.npos ? text.size() : end + 1);
    return line;
}

bool observed_source(const Session& session, std::string_view path, std::string_view label) {
    const auto first = session.tasks.empty() ? 0 : session.tasks.back().first_round;
    for (auto i = session.rounds.size(); i > first; --i) {
        const auto& tools = session.rounds[i - 1].tools;
        for (auto record = tools.rbegin(); record != tools.rend(); ++record) {
            if (!record->dispatched) {
                continue;
            }
            const bool same_file = text_arg(record->call, "path") == path;
            // A later command or edit can change lines from an earlier read.
            if (record->call.name == "run" || (record->call.name == "edit" && same_file)) {
                return false;
            }
            if (record->call.name != "read" || !same_file || record->result.status != ToolStatus::ok) {
                continue;
            }
            auto lines = std::string_view(record->result.text);
            if (take_line(lines) != path) {
                continue;
            }
            while (!lines.empty()) {
                const auto line = take_line(lines);
                if (line.starts_with(label)) {
                    return line.substr(label.size()) != "[line exceeds the 32768-byte tool result limit]";
                }
            }
        }
    }
    return false;
}

bool already_cited(std::string_view text, std::string_view citation) {
    constexpr auto path_characters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./\\-"sv;
    constexpr auto number_continuation = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"sv;
    for (auto pos = text.find(citation); pos != text.npos; pos = text.find(citation, pos + 1)) {
        const auto end = pos + citation.size();
        const auto prefix = pos >= 2 ? text.substr(pos - 2, 2) : std::string_view{};
        const auto start = prefix == "./" || prefix == ".\\" ? pos - 2 : pos;
        const bool decimal_suffix = end + 1 < text.size() && text[end] == '.' &&
            text[end + 1] >= '0' && text[end + 1] <= '9';
        if ((start == 0 || path_characters.find(text[start - 1]) == text.npos) &&
            (end == text.size() || number_continuation.find(text[end]) == text.npos) && !decimal_suffix) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<std::string> verified_source_citations(const Session& session) {
    std::vector<std::string> citations;
    if (!session.finished || session.stop != SessionStop::end_turn || session.rounds.empty()) {
        return citations;
    }
    const std::string_view answer = session.rounds.back().text;
    auto remaining = answer;
    constexpr std::size_t candidate_limit = 64;
    std::size_t candidates = 0;
    while (!remaining.empty() && candidates < candidate_limit) {
        auto path = field(take_line(remaining), {"文件", "路径", "file", "path"});
        if (path.starts_with("./") || path.starts_with(".\\")) {
            path.remove_prefix(2);
        }
        if (path.empty()) {
            continue;
        }
        auto next = remaining;
        auto number = field(take_line(next), {"行号", "行", "line", "line number"});
        if (number.starts_with("第")) {
            number.remove_prefix("第"sv.size());
        }
        if (number.ends_with("行")) {
            number.remove_suffix("行"sv.size());
        }
        number = trim(number);
        if (number.empty()) {
            continue;
        }
        std::size_t line = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), line);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || line == 0) {
            continue;
        }
        ++candidates;
        const auto decimal = std::to_string(line);
        const auto label = decimal + ": ";
        auto citation = std::string(path) + ':' + decimal;
        if (!already_cited(answer, citation) && std::ranges::find(citations, citation) == citations.end() &&
            observed_source(session, path, label)) {
            citations.push_back(std::move(citation));
        }
        remaining = next;
    }
    return citations;
}

} // namespace lay
