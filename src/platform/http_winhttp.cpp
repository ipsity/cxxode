#include "platform/http.hpp"
#include "base/utf.hpp"

#include <winhttp.h>

#include <memory>

namespace ode {
namespace {

struct handle_closer {
    void operator()(void* h) const { if (h) WinHttpCloseHandle(h); }
};
using hinternet = std::unique_ptr<void, handle_closer>;

std::string last_error(std::string_view what) {
    return std::string(what) + " failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

HINTERNET session_handle() {
    // One process-wide session keeps connections and TLS sessions warm across requests.
    static hinternet session{[] {
        auto* h = WinHttpOpen(L"ode/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h) h = WinHttpOpen(L"ode/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) {
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
        return h;
    }()};
    return session.get();
}

} // namespace

http_result http_send(const http_request& request, const http_sink& sink, cancel_token& cancel) {
    http_result result;
    auto* session = session_handle();
    if (!session) {
        result.error = last_error("WinHttpOpen");
        return result;
    }
    auto url = widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        result.error = "Invalid URL: " + request.url;
        return result;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    hinternet connection{WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
    if (!connection) {
        result.error = last_error("WinHttpConnect");
        return result;
    }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET raw = WinHttpOpenRequest(connection.get(), widen(request.method).c_str(), path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!raw) {
        result.error = last_error("WinHttpOpenRequest");
        return result;
    }
    // Closing the request handle from the UI thread aborts a blocked receive.
    if (!cancel.set_hook([raw] { WinHttpCloseHandle(raw); })) {
        WinHttpCloseHandle(raw);
        result.cancelled = true;
        return result;
    }
    const auto finish = [&] {
        if (cancel.clear_hook()) WinHttpCloseHandle(raw);
        if (cancel.cancelled()) result.cancelled = true;
        return result;
    };

    WinHttpSetTimeouts(raw, 0, 30000, 60000, 600000);
    std::wstring headers;
    for (const auto& [name, value] : request.headers) headers += widen(name + ": " + value + "\r\n");
    if (!WinHttpSendRequest(raw, headers.c_str(), static_cast<DWORD>(headers.size()),
                            const_cast<char*>(request.body.data()), static_cast<DWORD>(request.body.size()),
                            static_cast<DWORD>(request.body.size()), 0) ||
        !WinHttpReceiveResponse(raw, nullptr)) {
        if (!cancel.cancelled()) result.error = last_error("HTTP request");
        return finish();
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(raw, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);
    const bool ok = status >= 200 && status < 300;

    char buffer[16384];
    while (!cancel.cancelled()) {
        // Waiting on a full read can hold the first SSE deltas until the reply ends.
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(raw, &available)) {
            if (!cancel.cancelled()) result.error = last_error("Checking response data");
            break;
        }
        if (available == 0) break;
        DWORD received = 0;
        if (!WinHttpReadData(raw, buffer, std::min(available, static_cast<DWORD>(sizeof(buffer))), &received)) {
            if (!cancel.cancelled()) result.error = last_error("Reading the response");
            break;
        }
        if (received == 0) break;
        if (ok) {
            sink({buffer, received});
        } else if (result.body.size() < 65536) {
            result.body.append(buffer, received);
        }
    }
    return finish();
}

} // namespace ode
