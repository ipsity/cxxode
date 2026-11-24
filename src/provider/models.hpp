#pragma once

#include "provider/provider.hpp"

#include <string>
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

// List available models from a provider
std::vector<model_info> list_models(const provider_config& config);

} // namespace ode
