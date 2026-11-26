#include "app/builtin_commands.hpp"
#include "provider/models.hpp"
#include "ui/term.hpp"

#include <iostream>

namespace ode {

int cmd_models(app& a, const std::string&) {
    // List available models for current provider
    ui::write_stream(false, "Fetching models from " + a.config.provider + "...\n");

    const auto models = list_models(a.provider_cfg);

    if (models.empty()) {
        ui::write_stream(true, "No models found or provider doesn't support listing.\n");
        return 1;
    }

    ui::write_stream(false, "\nAvailable models:\n\n");

    for (const auto& m : models) {
        std::string line = "  " + m.id;
        if (!m.description.empty()) {
            line += "  - " + m.description;
        }
        if (m.context_window > 0) {
            line += "  (" + (m.context_window >= 1000000
                ? std::to_string(m.context_window / 1000000) + "M"
                : std::to_string(m.context_window / 1000) + "k") + " context)";
        }
        ui::write_stream(false, line + "\n");
    }

    ui::write_stream(false, "\nUse: ode --model <model-id>\n");
    return 0;
}

int cmd_model(app& a, const std::string& args) {
    if (args.empty()) {
        // Show current model
        ui::write_stream(false, "Current model: " + a.core->model + "\n");
        ui::write_stream(false, "Current provider: " + a.config.provider + "\n");
        ui::write_stream(false, "\nUse /models to list available models\n");
        return 0;
    }

    // Switch model
    a.set_model(args);
    ui::write_stream(false, "Switched to model: " + a.core->model + "\n");
    return 0;
}

} // namespace ode
