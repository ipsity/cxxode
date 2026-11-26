#pragma once

#include "provider/provider.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ode {

// Model information from provider API
struct model_info {
    std::string id;
    std::string name;
    std::string description;
    std::size_t context_window = 0;
    bool supports_vision = false;
    bool supports_tools = false;
};

struct model_list {
    std::vector<model_info> models;
    std::string error;
};

model_list list_models(const provider_config& config);
std::size_t known_model_context_window(const provider_config& config, std::string_view model);

} // namespace ode
