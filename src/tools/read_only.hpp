#pragma once

#include "core/session.hpp"
#include "platform/workspace.hpp"

namespace lay {

class ReadOnlyWorkspace {
public:
    ReadOnlyWorkspace(const std::filesystem::path& root, std::vector<std::string> excluded,
                      bool versions = false);
    std::string_view authorize(const ToolCall& call) const;
    ToolResult execute(const ToolCall& call, const StopRequested& stop_requested) const;

private:
    friend class EditWorkspace;
    platform::Node open(std::string_view path) const;
    bool allowed(std::string_view path) const;
    ToolResult read(const platform::Node& node, const ToolCall& call) const;
    ToolResult search(const platform::Node& node, const ToolCall& call,
                      const StopRequested& stop_requested) const;

    platform::Node root_;
    std::vector<std::string> excluded_;
    bool versions_;
};

} // namespace lay
