#pragma once

#include "core/session.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>

namespace lay::tui {

int run(std::span<const std::string> arguments);

enum class WorkspaceAction { prepare, inspect, start, follow_up, resume, close_task, close_unknown };

struct WorkspaceCommand {
    WorkspaceAction action;
    std::string question{};
    bool allow_edit = true;
};

struct CheckpointView {
    std::int64_t revision = -1;
    std::string_view storage_error{};
    std::string_view observed_reply{};
    Limits limits{};
};

struct WorkspacePort {
    StopRequested stopping;
    std::function<void(const Session&, std::string_view,
                       const std::function<std::vector<std::string>(std::size_t)>&,
                       std::span<const std::string>, CheckpointView)> checkpoint;
    std::function<void(std::string_view)> scope;
    std::function<void(std::string_view)> preview;
    std::function<std::optional<bool>(std::size_t, const ToolCall&, std::string, std::string)> review;
    std::function<std::optional<std::string>(std::size_t)> take_supplement;
    StopRequested supplement_requested;
};

struct WorkspaceScreen {
    std::string id;
    std::string path;
    std::string draft;
    bool resume;
    bool mono;
    bool ascii;
    bool writable;
};

using WorkspaceWork = std::function<void(const WorkspaceCommand&, const WorkspacePort&)>;
int run_workspace(const WorkspaceScreen& settings, const WorkspaceWork& work);

} // namespace lay::tui
