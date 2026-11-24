#include "provider/codec.hpp"
#include "provider/sse.hpp"
#include "platform/http.hpp"

#include <chrono>
#include <thread>

namespace lay {
namespace {

void feed(stream_parser& parser, std::string_view name, std::string_view data) {
    if (data == "[DONE]") return;
    auto value = json::parse(data, nullptr, false);
    if (!value.is_discarded()) parser.event(name, value);
}

std::string trim_slash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

std::string error_message(const http_result& r) {
    auto body = json::parse(r.body, nullptr, false);
    std::string detail = r.body.substr(0, 400);
    if (body.is_object() && body.contains("error")) {
        const auto& e = body["error"];
        detail = e.is_object() ? e.value("message", e.dump()) : e.dump();
    }
    return "HTTP " + std::to_string(r.status) + ": " + detail;
}

class http_provider final : public provider {
public:
    explicit http_provider(provider_config config) : config_(std::move(config)) {}

    model_reply send(const model_request& request, const stream_events& events, cancel_token& cancel) override {
        http_request http;
        http.url = trim_slash(config_.base_url);
        http.headers = {{"content-type", "application/json"}, {"accept", "text/event-stream"}};
        switch (config_.kind) {
            case protocol::anthropic:
                http.url += "/v1/messages";
                http.headers.emplace_back("anthropic-version", "2023-06-01");
                if (!config_.api_key.empty()) {
                    if (config_.bearer) http.headers.emplace_back("authorization", "Bearer " + config_.api_key);
                    else http.headers.emplace_back("x-api-key", config_.api_key);
                }
                break;
            case protocol::openai_chat:
            case protocol::openai_responses:
                if (!http.url.ends_with("/v1")) http.url += "/v1";
                http.url += config_.kind == protocol::openai_chat ? "/chat/completions" : "/responses";
                if (!config_.api_key.empty()) http.headers.emplace_back("authorization", "Bearer " + config_.api_key);
                break;
        }
        http.body = encode_request(config_.kind, request).dump();

        // Retry only before any byte of a reply was shown; a partial stream is reported, not replayed.
        for (int attempt = 0;; ++attempt) {
            auto parser = make_parser(config_.kind, events);
            sse_decoder decoder([&](std::string_view name, std::string_view data) { feed(*parser, name, data); });
            bool received = false;
            const auto result = http_send(http, [&](std::string_view chunk) { received = true; decoder.feed(chunk); }, cancel);
            if (result.cancelled) {
                model_reply reply = parser->finish();
                reply.stop = stop_reason::cancelled;
                reply.complete = false;
                reply.error = "Cancelled.";
                return reply;
            }
            const bool retryable = !received && attempt < 4 &&
                (result.status == 429 || result.status == 529 || result.status >= 500 || (result.status == 0 && !result.error.empty()));
            if (retryable) {
                const auto delay = std::chrono::milliseconds(500 << attempt);
                const auto until = std::chrono::steady_clock::now() + delay;
                while (std::chrono::steady_clock::now() < until && !cancel.cancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (!cancel.cancelled()) continue;
            }
            decoder.finish();
            model_reply reply = parser->finish();
            if (result.status != 0 && (result.status < 200 || result.status >= 300)) {
                reply.complete = false;
                reply.stop = stop_reason::error;
                reply.error = error_message(result);
            } else if (!result.error.empty()) {
                reply.complete = false;
                reply.stop = stop_reason::error;
                reply.error = result.error;
            }
            return reply;
        }
    }

private:
    provider_config config_;
};

} // namespace

std::unique_ptr<stream_parser> make_parser(protocol kind, const stream_events& events) {
    switch (kind) {
        case protocol::anthropic: return make_anthropic_parser(events);
        case protocol::openai_chat: return make_chat_parser(events);
        case protocol::openai_responses: return make_responses_parser(events);
    }
    return nullptr;
}

json encode_request(protocol kind, const model_request& request) {
    switch (kind) {
        case protocol::anthropic: return encode_anthropic(request);
        case protocol::openai_chat: return encode_chat(request);
        case protocol::openai_responses: return encode_responses(request);
    }
    return {};
}

model_reply decode_stream(protocol kind, std::string_view sse_bytes, const stream_events& events) {
    auto parser = make_parser(kind, events);
    sse_decoder decoder([&](std::string_view name, std::string_view data) { feed(*parser, name, data); });
    decoder.feed(sse_bytes);
    decoder.finish();
    return parser->finish();
}

std::unique_ptr<provider> make_provider(const provider_config& config) {
    return std::make_unique<http_provider>(config);
}

} // namespace lay
