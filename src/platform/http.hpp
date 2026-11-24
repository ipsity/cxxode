#pragma once

#include "core/cancel.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lay {

struct http_request {
    std::string method = "POST";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct http_result {
    int status = 0;       // 0 when no response arrived
    std::string error;    // transport failure, empty on success
    bool cancelled = false;
    std::string body;     // collected only when status is not 2xx
};

using http_sink = std::function<void(std::string_view chunk)>;

// Streams the 2xx response body to sink as it arrives.
http_result http_send(const http_request& request, const http_sink& sink, cancel_token& cancel);

} // namespace lay
