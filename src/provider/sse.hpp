#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace ode {

// Incremental text/event-stream decoder. Bytes may arrive split at any position.
class sse_decoder {
public:
    using handler = std::function<void(std::string_view event, std::string_view data)>;
    explicit sse_decoder(handler on_event) : on_event_(std::move(on_event)) {}

    void feed(std::string_view bytes) {
        buffer_.append(bytes);
        std::size_t start = 0;
        for (auto end = buffer_.find('\n', start); end != std::string::npos; end = buffer_.find('\n', start)) {
            auto line = std::string_view(buffer_).substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            this->line(line);
            start = end + 1;
        }
        buffer_.erase(0, start);
    }

    void finish() {
        if (!buffer_.empty()) feed("\n");
        line({});
    }

private:
    void line(std::string_view text) {
        if (text.empty()) {
            if (has_data_) on_event_(event_.empty() ? std::string_view("message") : std::string_view(event_), data_);
            event_.clear();
            data_.clear();
            has_data_ = false;
            return;
        }
        if (text.front() == ':') return;
        const auto colon = text.find(':');
        const auto field = text.substr(0, colon);
        auto value = colon == std::string_view::npos ? std::string_view{} : text.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (field == "event") {
            event_ = value;
        } else if (field == "data") {
            if (has_data_) data_ += '\n';
            data_ += value;
            has_data_ = true;
        }
    }

    handler on_event_;
    std::string buffer_;
    std::string event_;
    std::string data_;
    bool has_data_ = false;
};

} // namespace ode
