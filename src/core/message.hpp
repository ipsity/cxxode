#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace lay {

using json = nlohmann::json;

struct block {
    enum class kind { text, tool_use, tool_result, thinking, opaque };
    kind type = kind::text;
    std::string text;       // text, thinking, tool_result content
    std::string id;         // tool_use id, tool_result tool_use_id
    std::string name;       // tool_use name
    json input;             // tool_use input; opaque provider item
    std::string signature;  // thinking signature
    bool is_error = false;

    static block make_text(std::string text);
    static block make_tool_use(std::string id, std::string name, json input);
    static block make_tool_result(std::string id, std::string content, bool is_error);
};

struct message {
    enum class role { user, assistant };
    role from = role::user;
    std::vector<block> blocks;

    std::string plain_text() const;
    std::vector<const block*> tool_uses() const;
};

struct usage {
    std::size_t input = 0;
    std::size_t output = 0;
    std::size_t cache_read = 0;
    std::size_t cache_write = 0;
    std::size_t context() const { return input + cache_read + cache_write + output; }
};

json to_json(const message& value);
message message_from_json(const json& value);

} // namespace lay
