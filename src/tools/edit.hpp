#pragma once

#include "tools/read_only.hpp"

namespace lay {

enum class Approval { approved, denied, unavailable };
using ReviewEdit = std::function<Approval(const EditEvidence&, std::string_view)>;

class EditWorkspace {
public:
    EditWorkspace(const ReadOnlyWorkspace& workspace, std::string writable_path);

    std::string root_identity() const;
    EditPreparation prepare(const ToolCall& call, const Session& session,
                            const ReviewEdit& review, const StopRequested& stop) const;
    ToolResult recover(const EditEvidence& evidence) const;
    void cleanup(const EditEvidence& evidence) const;

private:
    platform::Node parent() const;
    bool current(const platform::Node& directory, const EditEvidence& evidence) const;
    ToolResult apply(const EditEvidence& evidence, std::string_view bytes,
                     const StopRequested& stop) const;

    const ReadOnlyWorkspace& workspace_;
    std::string path_;
    std::string dirname_;
    std::string filename_;
};

} // namespace lay
