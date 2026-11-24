#pragma once

#include <string>
#include <vector>

namespace ode::ui {

// Welcome screen for first-time users
std::vector<std::string> welcome_screen(int columns);

// Check if welcome should be shown
bool should_show_welcome();

// Mark welcome as shown
void mark_welcome_shown();

} // namespace ode::ui
