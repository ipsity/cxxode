#pragma once

#include "tui.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace lay::tui {

struct Card {
    std::string title;
    std::string detail;
};

struct Transcript {
    std::vector<Card> cards;
    std::size_t tasks;
    bool closed;
    SessionStop stop;
    std::string diagnostic;
};

struct Review {
    std::size_t ticket;
    std::size_t task;
    std::string call;
    std::string title;
    std::string detail;
};

struct WorkspaceView {
    std::shared_ptr<const Transcript> transcript;
    std::shared_ptr<const Review> review;
    std::string scope;
    std::string preview;
    std::string error;
    bool busy = false;
    bool requesting = false;
    bool stopping = false;
    bool operator==(const WorkspaceView&) const = default;
};

class WorkspaceWorker {
public:
    WorkspaceWorker(const WorkspaceWork& work, bool ascii);
    ~WorkspaceWorker();
    void launch(WorkspaceCommand command);
    WorkspaceView view() const;
    void stop();
    void decide(std::size_t ticket, bool approved);

private:
    void publish(const Session& snapshot, std::string_view point);
    std::optional<bool> review(std::size_t task, const ToolCall& call, std::string title, std::string detail);
    void execute(const WorkspaceCommand& command);

    const WorkspaceWork& work_;
    bool ascii_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    WorkspaceView view_;
    std::atomic_bool cancelled_ = false;
    std::size_t tickets_ = 0;
    std::optional<bool> decision_;
    std::jthread thread_;
};

std::string safe_text(std::string_view text, bool ascii, bool multiline = false);

} // namespace lay::tui
