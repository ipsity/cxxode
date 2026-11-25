#include "ext/commands.hpp"
#include "base/files.hpp"
#include "ext/discovery.hpp"

#include <algorithm>
#include <cctype>

namespace ode {

namespace fs = std::filesystem;

std::string user_command::load_body() const {
    if (!body.empty()) return body;
    return read_file(path).value_or("");
}

std::vector<user_command> load_commands(const fs::path& workspace) {
    std::vector<user_command> commands;
    for (const auto& dir : extension_dirs(workspace, "commands")) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;

        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto& path = entry.path();
            if (path.extension() != ".md") continue;

            const auto content = read_file(path);
            if (!content) continue;

            auto [meta, body] = parse_frontmatter(*content);
            user_command command{meta.contains("name") ? meta["name"] : path.stem().string(), meta["description"],
                                 path, std::move(body), meta["argument-hint"]};
            const auto found = std::ranges::find(commands, command.name, &user_command::name);
            if (found == commands.end()) commands.push_back(std::move(command));
            else *found = std::move(command);
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

std::string expand_command(std::string_view body, std::string_view arguments, std::string_view workspace) {
    std::map<std::string, std::string> vars{{"ARGUMENTS", std::string(arguments)}, {"WORKSPACE", std::string(workspace)}};
    std::size_t offset = 0;
    for (int index = 1; index <= 9; ++index) {
        offset = arguments.find_first_not_of(" \t\r\n", offset);
        if (offset == std::string_view::npos) break;
        std::string argument;
        bool quoted = false;
        for (; offset < arguments.size(); ++offset) {
            const char c = arguments[offset];
            if (c == '\\' && offset + 1 < arguments.size() && arguments[offset + 1] == '"') {
                argument += arguments[++offset];
            } else if (c == '"') {
                quoted = !quoted;
            } else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
                break;
            } else {
                argument += c;
            }
        }
        vars.emplace(std::to_string(index), std::move(argument));
    }
    return expand_variables(body, vars);
}

} // namespace ode
