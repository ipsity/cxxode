#pragma once

#include "core/session.hpp"
#include "platform/workspace.hpp"

namespace lay {

class ReadOnlyWorkspace {
public:
    ReadOnlyWorkspace(const std::filesystem::path& root, std::vector<std::string> excluded,
                      bool editing = false, bool persistent = false);
    std::string_view authorize(const ToolCall& call) const;
    ToolResult execute(const ToolCall& call, const StopRequested& stop_requested) const;
    bool matches_file_version(std::string_view path, std::string_view version) const;
    std::string root_identity() const { return root_.identity(); }

private:
    friend class EditWorkspace;
    friend class RunWorkspace;
    platform::Node open(std::string_view path) const;
    bool allowed(std::string_view path) const;
    ToolResult read(const platform::Node& node, const ToolCall& call) const;
    ToolResult search(const platform::Node& node, const ToolCall& call,
                      const StopRequested& stop_requested) const;

    platform::Node root_;
    std::vector<std::string> excluded_;
    bool editing_;
    bool persistent_;
};

} // namespace lay
