#include "tools/tool.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ode {
namespace {

std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> result;
    std::string word;
    for (const unsigned char ch : text) {
        if (std::isalnum(ch) || ch >= 0x80) {
            word.push_back(static_cast<char>(std::tolower(ch)));
        } else if (!word.empty()) {
            result.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty()) result.push_back(std::move(word));
    return result;
}

} // namespace

tool_registry::tool_registry(std::vector<tool> tools) : tools_(std::move(tools)), activated_(tools_.size()) {}

const tool* tool_registry::find(std::string_view name) const {
    const auto found = std::ranges::find(tools_, name, &tool::name);
    return found == tools_.end() ? nullptr : &*found;
}

std::vector<tool> tool_registry::model_tools() const {
    std::vector<tool> result;
    for (std::size_t i = 0; i < tools_.size(); ++i) {
        if (tools_[i].exposure == tool_exposure::direct || activated_[i]) result.push_back(tools_[i]);
    }
    return result;
}

std::vector<tool_search_result> tool_registry::search(std::string_view query, std::size_t limit, bool activate) {
    const auto terms = words(query);
    if (terms.empty() || limit == 0) return {};

    struct match {
        std::size_t index;
        std::size_t score;
    };
    std::vector<match> matches;
    for (std::size_t i = 0; i < tools_.size(); ++i) {
        const auto& candidate = tools_[i];
        if (candidate.name == "codemode" || candidate.name == "tool_search" ||
            (activate && (candidate.exposure != tool_exposure::deferred || activated_[i]))) continue;
        const auto name_words = words(candidate.name);
        const auto description_words = words(candidate.description);
        std::size_t score = 0;
        for (const auto& term : terms) {
            if (std::ranges::find(name_words, term) != name_words.end()) score += 4;
            if (std::ranges::find(description_words, term) != description_words.end()) score += 1;
        }
        if (score > 0) matches.push_back({i, score});
    }
    std::ranges::sort(matches, [&](const match& left, const match& right) {
        if (left.score != right.score) return left.score > right.score;
        return tools_[left.index].name < tools_[right.index].name;
    });

    std::vector<tool_search_result> result;
    const auto count = std::min(limit, matches.size());
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& matched = matches[i];
        if (activate) activated_[matched.index] = true;
        result.push_back({tools_[matched.index].name, tools_[matched.index].description, tools_[matched.index].schema});
    }
    return result;
}

tool make_tool_search_tool() {
    tool result;
    result.name = "tool_search";
    result.description = "Search for deferred tools and make matching tools available on the next turn.";
    result.schema = {
        {"type", "object"},
        {"properties", {
            {"query", {{"type", "string"}, {"description", "Words describing the tool you need"}}},
            {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}, {"description", "Maximum matches, default 5"}}}
        }},
        {"required", {"query"}},
        {"additionalProperties", false}
    };
    result.access = tool_access::read;
    result.run = [](const json& input, tool_context& context) {
        if (!context.search_tools) return tool_output{"Tool search is unavailable.", true, "failed", {}};
        const auto query = input.value("query", std::string{});
        const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(input.value("limit", std::int64_t{5}), 1, 20));
        const auto matches = context.search_tools(query, limit, true);
        if (matches.empty()) return tool_output{"No matching deferred tools found.", false, "0 tools", {}};
        std::string text = "Available on the next model turn:\n";
        for (const auto& match : matches) {
            const auto description = match.description.substr(0, match.description.find_first_of("\r\n"));
            text += "- " + match.name + ": " + description + "\n";
        }
        return tool_output{std::move(text), false, std::to_string(matches.size()) + " tools", {}};
    };
    return result;
}

} // namespace ode
