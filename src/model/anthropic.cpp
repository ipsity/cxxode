#include "model/anthropic.hpp"
#include "model/messages_codec.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lay {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class CurlGlobal {
public:
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("could not initialize libcurl");
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};

struct HeaderList {
    curl_slist* value = nullptr;

    ~HeaderList() { curl_slist_free_all(value); }

    void add(const std::string& header) {
        auto* extended = curl_slist_append(value, header.c_str());
        if (!extended) {
            throw std::runtime_error("could not allocate HTTP headers");
        }
        value = extended;
    }
};

struct ActiveEasy {
    CURLM* multi;
    CURL* easy;
    ~ActiveEasy() { curl_multi_remove_handle(multi, easy); }
};

std::string_view strip_header(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string_view::npos
        ? std::string_view{}
        : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

bool same_header(std::string_view value, std::string_view lower) {
    return std::ranges::equal(value, lower, [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a + ('a' - 'A')) : a) == b;
    });
}

bool safe_request_id(std::string_view id) {
    return id.size() <= 128 && std::ranges::all_of(id, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

struct Transfer {
    CURL* easy;
    messages::Stream stream;
    Clock::time_point last_activity = Clock::now();
    std::string request_id;
    std::optional<std::chrono::seconds> retry_after;
    std::size_t discarded = 0;
    bool invalid_content_type = false;
    bool callback_error = false;
};

std::size_t read_header(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
    auto& transfer = *static_cast<Transfer*>(context);
    const auto length = size * count;
    try {
        transfer.last_activity = Clock::now();
        const std::string_view line(bytes, length);
        if (line.starts_with("HTTP/")) {
            transfer.request_id.clear();
            transfer.retry_after.reset();
            return length;
        }
        const auto delimiter = line.find(':');
        if (delimiter == std::string_view::npos) {
            return length;
        }
        const auto key = line.substr(0, delimiter);
        const auto value = strip_header(line.substr(delimiter + 1));
        if (same_header(key, "request-id") && safe_request_id(value)) {
            transfer.request_id = value;
        } else if (same_header(key, "retry-after") && !value.empty()) {
            int seconds = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && seconds >= 0) {
                transfer.retry_after = std::chrono::seconds(seconds);
            } else if (parsed.ec == std::errc::result_out_of_range) {
                transfer.retry_after = 61s;
            } else {
                const auto date = curl_getdate(std::string(value).c_str(), nullptr);
                if (date >= 0) {
                    const auto delay = std::clamp(std::difftime(date, std::time(nullptr)), 0.0, 61.0);
                    transfer.retry_after = std::chrono::seconds(static_cast<long long>(delay));
                }
            }
        }
        return length;
    } catch (...) {
        transfer.callback_error = true;
        return 0;
    }
}

std::size_t read_body(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
    auto& transfer = *static_cast<Transfer*>(context);
    const auto length = size * count;
    try {
        transfer.last_activity = Clock::now();
        long status = 0;
        curl_easy_getinfo(transfer.easy, CURLINFO_RESPONSE_CODE, &status);
        if (status != 200) {
            transfer.discarded += length;
            return transfer.discarded <= 128 * 1024 ? length : 0;
        }
        char* content_type = nullptr;
        curl_easy_getinfo(transfer.easy, CURLINFO_CONTENT_TYPE, &content_type);
        const std::string_view mime = content_type ? content_type : "";
        if (!same_header(strip_header(mime.substr(0, mime.find(';'))), "text/event-stream")) {
            transfer.invalid_content_type = true;
            return 0;
        }
        return transfer.stream.push({bytes, length}) ? length : 0;
    } catch (...) {
        transfer.callback_error = true;
        return 0;
    }
}

struct Attempt {
    std::optional<GenerationFailure> failure;
    std::optional<std::chrono::milliseconds> retry_after;
};

class Transport {
public:
    Transport(std::string url, std::string key, bool loopback)
        : url_(std::move(url)), key_(std::move(key)), loopback_(loopback) {
        static const CurlGlobal initialized;
        if (!loopback_ && !(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_ASYNCHDNS)) {
            throw std::runtime_error("the configured libcurl lacks asynchronous DNS");
        }
        multi_.reset(curl_multi_init());
        if (!multi_) {
            throw std::runtime_error("could not initialize the libcurl multi handle");
        }
    }

    Attempt post(const std::string& request, const EventSink& emit, const StopRequested& stop_requested) {
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
        if (!easy) {
            throw std::runtime_error("could not initialize HTTP request");
        }
        Transfer transfer{easy.get(), {}, Clock::now(), {}, {}, 0, false, false};
        HeaderList headers;
        headers.add("content-type: application/json");
        headers.add("accept: text/event-stream");
        headers.add("anthropic-version: 2023-06-01");
        if (!loopback_) {
            headers.add("x-api-key: " + key_);
        }
        const auto set = [&](CURLoption option, auto value) {
            if (curl_easy_setopt(easy.get(), option, value) != CURLE_OK) {
                throw std::runtime_error("could not configure HTTP request");
            }
        };
        set(CURLOPT_URL, url_.c_str());
        set(CURLOPT_PROTOCOLS_STR, loopback_ ? "http" : "https");
        set(CURLOPT_FOLLOWLOCATION, 0L);
        set(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        set(CURLOPT_SSL_VERIFYPEER, 1L);
        set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        set(CURLOPT_TIMEOUT_MS, 90000L);
        if (loopback_) {
            set(CURLOPT_NOPROXY, "*");
        }
        set(CURLOPT_HTTPHEADER, headers.value);
        set(CURLOPT_POSTFIELDS, request.data());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.size()));
        set(CURLOPT_WRITEFUNCTION, &read_body);
        set(CURLOPT_WRITEDATA, &transfer);
        set(CURLOPT_HEADERFUNCTION, &read_header);
        set(CURLOPT_HEADERDATA, &transfer);

        if (curl_multi_add_handle(multi_.get(), easy.get()) != CURLM_OK) {
            throw std::runtime_error("could not start model request");
        }
        const ActiveEasy active{multi_.get(), easy.get()};
        int running = 0;
        do {
            if (stop_requested()) {
                return {GenerationFailure{SessionStop::cancelled, {}}, {}};
            }
            if (curl_multi_perform(multi_.get(), &running) != CURLM_OK) {
                return {GenerationFailure{SessionStop::transport_error, "HTTP transfer failed"}, {}};
            }
            if (running != 0 && Clock::now() - transfer.last_activity >= 30s) {
                return {GenerationFailure{SessionStop::transport_error, "model response idle timeout"}, 1ms};
            }
            if (running != 0 && curl_multi_poll(multi_.get(), nullptr, 0, 50, nullptr) != CURLM_OK) {
                return {GenerationFailure{SessionStop::transport_error, "HTTP poll failed"}, {}};
            }
        } while (running != 0);

        if (stop_requested()) {
            return {GenerationFailure{SessionStop::cancelled, {}}, {}};
        }
        int queued = 0;
        const auto* done = curl_multi_info_read(multi_.get(), &queued);
        if (!done || done->msg != CURLMSG_DONE || done->easy_handle != easy.get()) {
            return {GenerationFailure{SessionStop::transport_error, "HTTP transfer has no completion"}, {}};
        }

        const auto diagnostic = [&](std::string message) {
            if (!transfer.request_id.empty()) {
                message += " (request-id " + transfer.request_id + ")";
            }
            return message;
        };
        long status = 0;
        curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
        if (status != 0 && status != 200) {
            Attempt result{GenerationFailure{SessionStop::http_error, diagnostic("HTTP " + std::to_string(status))}, {}};
            if (status == 429 || status == 500 || status == 502 || status == 503 || status == 504 || status == 529) {
                result.retry_after = transfer.retry_after.value_or(0s);
            }
            return result;
        }
        if (transfer.stream.failure()) {
            auto failure = *transfer.stream.failure();
            failure.diagnostic = diagnostic(std::move(failure.diagnostic));
            return {std::move(failure), {}};
        }
        if (transfer.invalid_content_type || transfer.callback_error) {
            return {GenerationFailure{SessionStop::protocol_error, diagnostic("invalid model response")}, {}};
        }
        if (done->data.result != CURLE_OK) {
            const auto code = done->data.result;
            const bool transient = code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
                code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR;
            return {GenerationFailure{SessionStop::transport_error, diagnostic(curl_easy_strerror(code))},
                    transient ? std::optional(0ms) : std::nullopt};
        }
        return {transfer.stream.finish(emit), {}};
    }

private:
    std::string url_;
    std::string key_;
    bool loopback_;
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi_{nullptr, curl_multi_cleanup};
};

Generate make_model(std::string model, std::string url, std::string key, bool loopback,
                    StopRequested stop_requested) {
    auto transport = std::make_shared<Transport>(std::move(url), std::move(key), loopback);
    return [model = std::move(model), transport = std::move(transport),
            stop_requested = std::move(stop_requested)](const Request& request, const EventSink& emit) {
        Generation generation{0, {}};
        try {
            const auto body = messages::encode(request, model);
            if (body.size() > 192 * 1024) {
                generation.failure = GenerationFailure{SessionStop::request_limit, "request exceeds 196608 bytes"};
                return generation;
            }
            const auto limit = std::min<std::size_t>(3, request.attempts_left);
            for (std::size_t attempt = 0; attempt < limit; ++attempt) {
                ++generation.attempts;
                auto result = transport->post(body, emit, stop_requested);
                generation.failure = std::move(result.failure);
                if (!result.retry_after || attempt + 1 == limit || *result.retry_after > 60s) {
                    return generation;
                }
                std::uniform_int_distribution<int> jitter(0, 75);
                static thread_local std::mt19937 random(std::random_device{}());
                const auto delay = std::max(*result.retry_after,
                    std::chrono::milliseconds(150 * (1 << attempt) + jitter(random)));
                const auto until = Clock::now() + delay;
                while (Clock::now() < until && !stop_requested()) {
                    std::this_thread::sleep_until(std::min(until, Clock::now() + 25ms));
                }
                if (stop_requested()) {
                    generation.failure = GenerationFailure{SessionStop::cancelled, {}};
                    return generation;
                }
            }
        } catch (const std::exception&) {
            generation.failure = GenerationFailure{SessionStop::transport_error, "could not build or issue model request"};
        }
        return generation;
    };
}

} // namespace

Generate anthropic_model(std::string model, StopRequested stop_requested) {
    const auto* key = std::getenv("ANTHROPIC_API_KEY");
    if (!key || !*key) {
        throw std::invalid_argument("Set ANTHROPIC_API_KEY to use the Anthropic Messages API.");
    }
    const std::string_view token(key);
    if (token.size() > 1024 || !std::ranges::all_of(token, [](unsigned char c) { return c > 32 && c < 127; })) {
        throw std::invalid_argument("ANTHROPIC_API_KEY contains invalid characters.");
    }
    return make_model(std::move(model), "https://api.anthropic.com/v1/messages", key, false,
                      std::move(stop_requested));
}

Generate messages::loopback_model(std::string model, std::uint16_t port, StopRequested stop_requested) {
    return make_model(std::move(model), "http://127.0.0.1:" + std::to_string(port) + "/v1/messages", {}, true,
                      std::move(stop_requested));
}

} // namespace lay
