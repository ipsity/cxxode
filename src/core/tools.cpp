#include "core/tools.hpp"

#include <algorithm>

namespace lay {
namespace {

const ArgumentValue* argument(const ToolCall& call, std::string_view name) {
    const auto found = std::ranges::find(call.arguments, name, &Argument::name);
    return found == call.arguments.end() ? nullptr : &found->value;
}

} // namespace

std::string_view text_arg(const ToolCall& call, std::string_view name) {
    const auto* value = std::get_if<std::string>(argument(call, name));
    return value ? std::string_view(*value) : std::string_view{};
}

std::size_t number_arg(const ToolCall& call, std::string_view name, std::size_t fallback) {
    const auto* value = std::get_if<std::int64_t>(argument(call, name));
    return value ? static_cast<std::size_t>(*value) : fallback;
}

std::string_view validate(const ToolCall& call, bool edit_enabled) {
    const auto spec = std::ranges::find(tool_specs, call.name, &ToolSpec::name);
    if (spec == std::end(tool_specs) || (call.name == "edit" && !edit_enabled)) {
        return "unsupported_tool";
    }
    std::size_t recognized = 0;
    for (const auto& parameter : spec->parameters) {
        const auto* value = argument(call, parameter.name);
        if (!value && !parameter.required) {
            continue;
        }
        if (!value) {
            return "missing_tool_argument";
        }
        ++recognized;
        if (parameter.integer) {
            const auto* number = std::get_if<std::int64_t>(value);
            if (!number || *number < parameter.minimum || *number > parameter.maximum) {
                return "invalid_integer_argument";
            }
        } else {
            const auto* text = std::get_if<std::string>(value);
            if (!text || text->size() < static_cast<std::size_t>(parameter.minimum) ||
                text->size() > static_cast<std::size_t>(parameter.maximum) ||
                text->find('\0') != std::string::npos ||
                (!parameter.multiline && text->find_first_of("\r\n") != std::string::npos)) {
                return "invalid_text_argument";
            }
        }
    }
    return recognized == call.arguments.size() ? "" : "unexpected_tool_argument";
}

} // namespace lay
