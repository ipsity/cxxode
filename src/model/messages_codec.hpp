#pragma once

#include "model/provider.hpp"

namespace lay::messages {

struct ContextLimits {
    std::size_t window_tokens = 200000;
    std::size_t output_tokens = 2048;
    std::size_t safety_tokens = 2048;
    std::size_t body_bytes = 192 * 1024;
};

struct Projection {
    std::string body;
    ModelAttempt layout;
    std::string diagnostic;
};

Projection project(const Request& request, std::string_view model, ContextLimits limits = {});
Projection project(const Request& request, const Provider& provider, ContextLimits limits = {});
bool rejected_context(std::string_view error_body);

class Stream {
public:
    explicit Stream(Protocol protocol = Protocol::anthropic, const TextPreview& preview = {})
        : protocol_(protocol), preview_(preview) {}
    bool push(std::string_view bytes);
    std::optional<GenerationFailure> finish(const EventSink& emit);
    const std::optional<GenerationFailure>& failure() const { return failure_; }
    std::optional<std::size_t> input_tokens() const;
    std::optional<std::size_t> output_tokens() const;
    const std::optional<UsageFields>& provider_usage() const { return provider_usage_; }

private:
    bool line();
    bool event();
    bool chat_event();
    bool responses_event();
    bool fail(SessionStop stop, std::string_view detail);
    void preview_text(std::string_view text) noexcept;

    struct Block {
        enum class Kind { text, tool } kind;
        std::string id;
        std::string tool;
        std::string bytes;
        bool initial_input = false;
    };

    struct ChatCall {
        std::string id;
        std::string name;
        std::string arguments;
    };

    struct ResponseItem {
        std::string id;
        std::string type;
        bool closed = false;
    };

    Protocol protocol_;
    TextPreview preview_;
    std::string line_;
    std::string event_;
    std::string data_;
    std::vector<ModelEvent> events_;
    std::vector<std::string> tool_ids_;
    std::vector<ChatCall> chat_calls_;
    std::string chat_text_;
    std::vector<ResponseItem> response_items_;
    std::string response_id_;
    bool done_marker_ = false;
    bool preview_sent_ = false;
    std::optional<Block> block_;
    std::optional<ReplyStop> stop_;
    std::optional<GenerationFailure> failure_;
    std::optional<UsageFields> provider_usage_;
    std::size_t next_index_ = 0;
    std::size_t wire_size_ = 0;
    bool first_line_ = true;
    bool previous_cr_ = false;
    bool started_ = false;
    bool ended_ = false;
};

} // namespace lay::messages
