#pragma once

#include "core/session.hpp"

namespace lay::messages {

std::string encode(const Request& request, std::string_view model);

class Stream {
public:
    bool push(std::string_view bytes);
    std::optional<GenerationFailure> finish(const EventSink& emit);
    const std::optional<GenerationFailure>& failure() const { return failure_; }

private:
    bool line();
    bool event();
    bool fail(SessionStop stop, std::string_view detail);

    struct Block {
        enum class Kind { text, tool } kind;
        std::string id;
        std::string tool;
        std::string bytes;
        bool initial_input = false;
    };

    std::string line_;
    std::string event_;
    std::string data_;
    std::vector<ModelEvent> events_;
    std::vector<std::string> tool_ids_;
    std::optional<Block> block_;
    std::optional<ReplyStop> stop_;
    std::optional<GenerationFailure> failure_;
    std::size_t next_index_ = 0;
    std::size_t wire_size_ = 0;
    bool first_line_ = true;
    bool previous_cr_ = false;
    bool started_ = false;
    bool ended_ = false;
};

} // namespace lay::messages
