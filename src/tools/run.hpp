#pragma once

#include "tools/edit.hpp"

namespace lay {

using ReviewRun = std::function<Approval(const RunEvidence&)>;

class RunWorkspace {
public:
    explicit RunWorkspace(const ReadOnlyWorkspace& workspace) : workspace_(workspace) {}
    RunPreparation prepare(const ToolCall& call, const Session& session,
                           const ReviewRun& review, const StopRequested& stop) const;

private:
    const ReadOnlyWorkspace& workspace_;
};

} // namespace lay
