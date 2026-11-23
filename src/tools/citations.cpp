#include "tools/citations.hpp"

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <set>

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

std::optional<SourceCitation> observed_source(const Session& session, std::size_t answer_round,
                                               std::string path, std::string line, bool inline_form) {
    const auto label = line + ": ";
    const auto first = session.tasks.empty() ? 0 : session.tasks.back().first_round;
    bool historical = answer_round < first;
    for (auto i = answer_round; i > 0; --i) {
        const auto& tools = session.rounds[i - 1].tools;
        for (auto record = tools.rbegin(); record != tools.rend(); ++record) {
            if (!record->dispatched) {
                continue;
            }
            const bool same_file = text_arg(record->call, "path") == path;
            // A later command or edit can change lines from an earlier read.
            if (record->call.name == "run" || (record->call.name == "edit" && same_file)) {
                historical = true;
            }
            if (record->call.name != "read" || !same_file || record->result.status != ToolStatus::ok) {
                continue;
            }
            auto lines = std::string_view(record->result.text);
            if (take_line(lines) != path) {
                continue;
            }
            while (!lines.empty()) {
                const auto content = take_line(lines);
                if (content.starts_with(label) &&
                    content.substr(label.size()) != "[line exceeds the 32768-byte tool result limit]") {
                    const auto task = std::ranges::upper_bound(session.tasks, i - 1, {}, &Task::first_round)
                                      - session.tasks.begin();
                    return SourceCitation{std::move(path), std::move(line), record->result.version,
                        record->result.observed_utc, static_cast<std::size_t>(task), i,
                        historical || i - 1 < first, inline_form};
                }
            }
        }
    }
    return std::nullopt;
}

bool citation_at(std::string_view text, std::size_t pos, std::size_t end) {
    constexpr auto path_characters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./\\-"sv;
    constexpr auto number_continuation = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"sv;
    const auto prefix = pos >= 2 ? text.substr(pos - 2, 2) : std::string_view{};
    const auto start = prefix == "./" || prefix == ".\\" ? pos - 2 : pos;
    const bool decimal_suffix = end + 1 < text.size() && text[end] == '.' &&
        text[end + 1] >= '0' && text[end + 1] <= '9';
    return (start == 0 || path_characters.find(text[start - 1]) == text.npos) &&
        (end == text.size() || number_continuation.find(text[end]) == text.npos) && !decimal_suffix;
}

bool already_cited(std::string_view text, std::string_view citation) {
    for (auto pos = text.find(citation); pos != text.npos; pos = text.find(citation, pos + 1)) {
        if (citation_at(text, pos, pos + citation.size())) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<SourceCitation> source_citations(const Session& session, std::size_t answer_round) {
    std::vector<SourceCitation> citations;
    const std::string_view answer = session.rounds[answer_round].text;
    auto remaining = answer;
    constexpr std::size_t candidate_limit = 64;
    std::size_t candidates = 0;
    const auto add = [&](std::string_view path, std::string_view line, bool inline_form) {
        if (std::ranges::none_of(citations, [&](const SourceCitation& source) {
                return source.path == path && source.line == line;
            })) {
            if (auto source = observed_source(session, answer_round, std::string(path), std::string(line), inline_form)) {
                citations.push_back(std::move(*source));
            }
        }
    };
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
        add(path, decimal, already_cited(answer, std::string(path) + ':' + decimal));
        remaining = next;
    }
    if (session.tasks.empty()) {
        return citations;
    }
    std::set<std::string_view> paths;
    for (const auto& round : std::span(session.rounds).first(answer_round)) {
        for (const auto& tool : round.tools) {
            if (tool.dispatched && tool.call.name == "read" && tool.result.status == ToolStatus::ok) {
                paths.insert(text_arg(tool.call, "path"));
            }
        }
    }
    for (const auto path : paths) {
        const auto prefix = std::string(path) + ':';
        for (auto pos = answer.find(prefix); pos != answer.npos && candidates < candidate_limit;
             pos = answer.find(prefix, pos + prefix.size())) {
            ++candidates;
            const auto start = pos + prefix.size();
            const auto line = answer.substr(start, answer.find_first_not_of("0123456789", start) - start);
            if (!line.empty() && line.front() != '0' && citation_at(answer, pos, start + line.size())) {
                add(path, line, true);
            }
        }
    }
    return citations;
}

} // namespace lay
