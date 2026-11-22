#include "platform/process.hpp"
#include "core/utf8.hpp"

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace lay::platform {
namespace {

std::string display_bytes(std::string_view raw) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    result.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        const auto byte = static_cast<unsigned char>(raw[i]);
        if (byte == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') {
            ++i;
            continue;
        }
        const auto width = byte < 0xc2 ? 0u : byte < 0xe0 ? 2u : byte < 0xf0 ? 3u : 4u;
        if ((byte >= 32 && byte < 127) || byte == '\t' || byte == '\n') {
            result += raw[i++];
        } else if (width && width <= raw.size() - i && valid_utf8(raw.substr(i, width)) &&
                   !(byte == 0xc2 && static_cast<unsigned char>(raw[i + 1]) <= 0x9f)) {
            result += raw.substr(i, width);
            i += width;
        } else {
            result += "\\x";
            result += hex[byte >> 4];
            result += hex[byte & 15];
            ++i;
        }
    }
    return result;
}

} // namespace

TextList child_environment() {
#ifdef _WIN32
    constexpr std::wstring_view names[] = {
        L"COMSPEC", L"INCLUDE", L"LIB", L"LIBPATH", L"NUMBER_OF_PROCESSORS", L"PATH", L"PATHEXT",
        L"PROCESSOR_ARCHITECTURE", L"SystemRoot", L"TEMP", L"TMP", L"UCRTVersion",
        L"UniversalCRTSdkDir", L"VCINSTALLDIR", L"VCToolsInstallDir", L"WINDIR", L"WindowsSdkDir",
        L"WindowsSDKVersion",
    };
    TextList environment;
    for (const auto name : names) {
        if (const auto* value = _wgetenv(name.data()); value && *value) {
            const auto entry = std::filesystem::path(std::wstring(name) + L"=" + value).u8string();
            environment.emplace_back(entry.begin(), entry.end());
        }
    }
    std::ranges::sort(environment, [](const auto& a, const auto& b) {
        const auto A = path_from_utf8(a).wstring();
        const auto B = path_from_utf8(b).wstring();
        return CompareStringOrdinal(A.data(), static_cast<int>(A.size()), B.data(),
                                    static_cast<int>(B.size()), TRUE) == CSTR_LESS_THAN;
    });
#else
    constexpr const char* names[] = {"PATH", "LANG", "LC_ALL", "LC_CTYPE", "TMPDIR", "TERM"};
    TextList environment;
    for (const auto* name : names) {
        if (const auto* value = std::getenv(name); value && *value) {
            environment.push_back(std::string(name) + "=" + value);
        }
    }
#endif
    return environment;
}

void Capture::append(std::string_view bytes) {
    total_ += bytes.size();
    const auto first = std::min(bytes.size(), capacity - head_.size());
    head_ += bytes.substr(0, first);
    bytes.remove_prefix(first);
    if (bytes.size() >= capacity) {
        std::ranges::copy(bytes.substr(bytes.size() - capacity), tail_.begin());
        retained_ = capacity;
        cursor_ = 0;
        return;
    }
    const auto contiguous = std::min(bytes.size(), capacity - cursor_);
    std::ranges::copy(bytes.substr(0, contiguous), tail_.begin() + cursor_);
    std::ranges::copy(bytes.substr(contiguous), tail_.begin());
    cursor_ = (cursor_ + bytes.size()) % capacity;
    retained_ = std::min(capacity, retained_ + bytes.size());
}

CapturedOutput Capture::finish() const {
    const auto start = retained_ == capacity ? cursor_ : 0;
    const auto contiguous = std::min(retained_, capacity - start);
    auto tail = std::string(tail_.data() + start, contiguous);
    tail.append(tail_.data(), retained_ - contiguous);
    return {display_bytes(head_), display_bytes(tail), total_ - head_.size() - retained_, total_};
}

} // namespace lay::platform
