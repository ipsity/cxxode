#include "model/provider.hpp"
#include "model/messages_codec.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

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

void initialize_curl() {
    static const CurlGlobal initialized;
}

using EasyHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

template <typename Value>
void option(CURL* easy, CURLoption key, Value value) {
    if (curl_easy_setopt(easy, key, value) != CURLE_OK) {
        throw std::runtime_error("could not configure HTTP request");
    }
}

void common_http_options(CURL* easy, const Provider& provider, std::string_view url) {
    option(easy, CURLOPT_URL, url.data());
    option(easy, CURLOPT_PROTOCOLS_STR, provider.loopback ? "http,https" : "https");
    option(easy, CURLOPT_FOLLOWLOCATION, 0L);
    option(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    option(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    option(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    option(easy, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    option(easy, CURLOPT_NOSIGNAL, 1L);
    option(easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    if (provider.loopback) option(easy, CURLOPT_NOPROXY, "*");
}

void auth_headers(HeaderList& headers, const Provider& provider, std::string_view token) {
    if (provider.protocol == Protocol::anthropic) {
        headers.add("anthropic-version: 2023-06-01");
        if (!token.empty()) headers.add("x-api-key: " + std::string(token));
    } else if (!token.empty()) {
        headers.add("authorization: Bearer " + std::string(token));
    }
}

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
    std::string error_body{};
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
        if ((same_header(key, "request-id") || same_header(key, "x-request-id")) && safe_request_id(value)) {
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
            if (status == 400 && transfer.error_body.size() < 8192) {
                transfer.error_body.append(bytes, std::min(length, 8192 - transfer.error_body.size()));
            }
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
    std::optional<std::size_t> input_tokens{};
    std::optional<std::size_t> output_tokens{};
};

class Transport {
public:
    explicit Transport(Provider provider)
        : provider_(std::move(provider)), key_(provider_key(provider_)) {
        initialize_curl();
        if (!provider_.loopback && !(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_ASYNCHDNS)) {
            throw std::runtime_error("the configured libcurl lacks asynchronous DNS");
        }
        multi_.reset(curl_multi_init());
        if (!multi_) {
            throw std::runtime_error("could not initialize the libcurl multi handle");
        }
    }

    Attempt post(const std::string& request, const EventSink& emit, const StopRequested& stop_requested) {
        EasyHandle easy(curl_easy_init(), curl_easy_cleanup);
        if (!easy) {
            throw std::runtime_error("could not initialize HTTP request");
        }
        Transfer transfer{easy.get(), messages::Stream(provider_.protocol), Clock::now(), {}, {}, 0, false, false};
        HeaderList headers;
        headers.add("content-type: application/json");
        headers.add("accept: text/event-stream");
        auth_headers(headers, provider_, key_);
        const auto set = [&](CURLoption option, auto value) {
            if (curl_easy_setopt(easy.get(), option, value) != CURLE_OK) {
                throw std::runtime_error("could not configure HTTP request");
            }
        };
        common_http_options(easy.get(), provider_, provider_.endpoint);
        set(CURLOPT_TIMEOUT_MS, 90000L);
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
            const auto reason = status == 400 && messages::rejected_context(transfer.error_body)
                ? SessionStop::context_limit : SessionStop::http_error;
            Attempt result{GenerationFailure{reason, diagnostic("HTTP " + std::to_string(status))}, {}};
            if (status == 429 || status == 500 || status == 502 || status == 503 || status == 504 || status == 529) {
                result.retry_after = transfer.retry_after.value_or(0s);
            }
            return result;
        }
        if (transfer.stream.failure()) {
            auto failure = *transfer.stream.failure();
            failure.diagnostic = diagnostic(std::move(failure.diagnostic));
            return {std::move(failure), {}, transfer.stream.input_tokens(), transfer.stream.output_tokens()};
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
        return {transfer.stream.finish(emit), {}, transfer.stream.input_tokens(), transfer.stream.output_tokens()};
    }

private:
    Provider provider_;
    std::string key_;
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi_{nullptr, curl_multi_cleanup};
};

} // namespace

Provider resolve_provider(Provider provider) {
    initialize_curl();
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || provider.base_url.empty() || provider.base_url.size() > 1536 ||
        std::ranges::any_of(provider.base_url, [](unsigned char c) { return c <= 32 || c >= 127; }) ||
        curl_url_set(url.get(), CURLUPART_URL, provider.base_url.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) {
        throw std::invalid_argument("Use an absolute provider Base URL without embedded credentials.");
    }
    const auto part = [&](CURLUPart key) -> std::optional<std::string> {
        char* value = nullptr;
        if (curl_url_get(url.get(), key, &value, 0) != CURLUE_OK) {
            return std::nullopt;
        }
        std::unique_ptr<char, decltype(&curl_free)> owned(value, curl_free);
        return std::string(value);
    };
    auto host = part(CURLUPART_HOST).value_or("");
    const auto scheme = part(CURLUPART_SCHEME).value_or("");
    for (auto& c : host) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    provider.loopback = host == "127.0.0.1" || host == "[::1]";
    if (host.empty() || (scheme != "https" && !(scheme == "http" && provider.loopback)) ||
        part(CURLUPART_QUERY) || part(CURLUPART_FRAGMENT)) {
        throw std::invalid_argument("Provider URLs require HTTPS or a numeric loopback address, without a query or fragment.");
    }
    if (curl_url_set(url.get(), CURLUPART_HOST, host.c_str(), 0) != CURLUE_OK) {
        throw std::invalid_argument("Invalid provider host.");
    }
    const std::string_view endpoint = provider.protocol == Protocol::responses ? "/responses" :
        provider.protocol == Protocol::chat_completions ? "/chat/completions" : "/messages";
    auto path = part(CURLUPART_PATH).value_or("");
    while (path.ends_with('/')) path.pop_back();
    if (path.ends_with(endpoint)) {
        path.resize(path.size() - endpoint.size());
    }
    if (path.ends_with("/responses") || path.ends_with("/chat/completions") || path.ends_with("/messages")) {
        throw std::invalid_argument("Base URL endpoint does not match the provider protocol.");
    }
    if (path.empty()) path = "/v1";
    if (curl_url_set(url.get(), CURLUPART_PATH, path.c_str(), 0) != CURLUE_OK) {
        throw std::invalid_argument("Invalid provider URL path.");
    }
    provider.base_url = part(CURLUPART_URL).value_or("");
    if (provider.base_url.empty()) throw std::invalid_argument("Invalid provider URL.");
    provider.endpoint = provider.base_url + std::string(endpoint);
    return provider;
}

std::vector<std::string> list_models(const Provider& provider) {
    initialize_curl();
    const auto key = provider_key(provider);
    EasyHandle request(curl_easy_init(), curl_easy_cleanup);
    if (!request) {
        throw std::runtime_error("could not initialize model catalog request");
    }
    HeaderList headers;
    headers.add("accept: application/json");
    auth_headers(headers, provider, key);
    const auto url = provider.base_url + "/models";
    common_http_options(request.get(), provider, url);
    option(request.get(), CURLOPT_TIMEOUT_MS, 15000L);
    option(request.get(), CURLOPT_HTTPHEADER, headers.value);
    std::string response;
    const auto append = +[](char* bytes, std::size_t size, std::size_t count, void* target) noexcept -> std::size_t {
        try {
            auto& body = *static_cast<std::string*>(target);
            const auto length = size * count;
            if (length <= 1024 * 1024 - body.size()) {
                body.append(bytes, length);
                return length;
            }
        } catch (...) {}
        return 0;
    };
    option(request.get(), CURLOPT_WRITEFUNCTION, append);
    option(request.get(), CURLOPT_WRITEDATA, &response);
    const auto result = curl_easy_perform(request.get());
    long status = 0;
    curl_easy_getinfo(request.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 0 && status != 200) {
        throw std::runtime_error("Model catalog returned HTTP " + std::to_string(status) + ".");
    }
    if (result != CURLE_OK) {
        throw std::runtime_error("Could not read the model catalog within the request and 1 MiB limits.");
    }
    try {
        const auto payload = nlohmann::json::parse(response);
        const auto& data = payload.at("data");
        if (!data.is_array() || data.size() > 4096) {
            throw std::runtime_error("Invalid or excessive model catalog.");
        }
        std::vector<std::string> models;
        for (const auto& item : data) {
            const auto model = item.at("id").get<std::string>();
            if (!valid_model_id(model)) {
                throw std::runtime_error("Model catalog returned an invalid model ID.");
            }
            models.push_back(model);
        }
        std::ranges::sort(models);
        models.erase(std::unique(models.begin(), models.end()), models.end());
        return models;
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Model catalog did not return a valid OpenAI-style data array.");
    }
}

Generate provider_model(const Provider& provider, StopRequested stop_requested) {
    if (provider.model.empty()) {
        throw std::invalid_argument("Choose --model MODEL or set model in the provider profile. Use --list-models to see available IDs.");
    }
    auto transport = std::make_shared<Transport>(provider);
    return [provider, transport = std::move(transport),
            stop_requested = std::move(stop_requested)](const Request& request, const EventSink& emit) {
        Generation generation{0, {}};
        try {
            auto projection = messages::project(request, provider);
            if (projection.body.empty()) {
                generation.failure = GenerationFailure{SessionStop::context_limit, std::move(projection.diagnostic)};
                return generation;
            }
            const auto limit = std::min<std::size_t>(3, request.attempts_left);
            bool repaired = false;
            for (std::size_t attempt = 0; attempt < limit; ++attempt) {
                ++generation.attempts;
                auto result = transport->post(projection.body, emit, stop_requested);
                auto layout = projection.layout;
                layout.input_tokens = result.input_tokens;
                layout.output_tokens = result.output_tokens;
                generation.observations.push_back(layout);
                generation.failure = std::move(result.failure);
                if (!repaired && generation.failure && generation.failure->reason == SessionStop::context_limit &&
                    attempt + 1 < limit && request.history.size() > 1) {
                    messages::ContextLimits smaller;
                    smaller.body_bytes = projection.body.size() / 2;
                    auto candidate = messages::project(request, provider, smaller);
                    if (!candidate.body.empty()) {
                        repaired = true;
                        projection = std::move(candidate);
                        continue;
                    }
                    generation.failure->diagnostic += ". " + candidate.diagnostic;
                }
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

Generate messages::loopback_model(std::string model, std::uint16_t port, StopRequested stop_requested) {
    Provider provider;
    provider.model = std::move(model);
    provider.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/v1/messages";
    provider.loopback = true;
    return provider_model(provider, std::move(stop_requested));
}

} // namespace lay
