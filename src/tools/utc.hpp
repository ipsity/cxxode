#pragma once

#include <ctime>
#include <string>

namespace lay {

inline std::string utc_now() {
    const auto now = std::time(nullptr);
    std::tm calendar{};
#ifdef _WIN32
    if (gmtime_s(&calendar, &now) != 0) {
        return {};
    }
#else
    if (!gmtime_r(&now, &calendar)) {
        return {};
    }
#endif
    char timestamp[32]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &calendar);
    return timestamp;
}

} // namespace lay
