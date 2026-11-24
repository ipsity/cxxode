#include "core/session.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
#include <set>

namespace lay {

namespace fs = std::filesystem;

session_log::~session_log() {
    if (file_) std::fclose(file_);
}

bool session_log::open(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
#ifdef _WIN32
    file_ = _wfopen(path.c_str(), L"ab");
#else
    file_ = std::fopen(path.c_str(), "ab");
#endif
    path_ = path;
    return file_ != nullptr;
}

void session_log::append(const json& line) {
    if (!file_) return;
    auto text = line.dump(-1, ' ', false, json::error_handler_t::replace);
    text += '\n';
    std::fwrite(text.data(), 1, text.size(), file_);
    std::fflush(file_);
}

void session_log::append_message(const message& m) {
    append({{"v", 1}, {"type", "message"}, {"message", to_json(m)}});
}

fs::path lay_home() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return fs::path(home ? home : ".") / ".lay";
}

fs::path sessions_dir(const fs::path& workspace) {
    auto text = workspace.lexically_normal().generic_u8string();
    std::string slug;
    for (const auto c : text) {
        const auto u = static_cast<unsigned char>(c);
        slug += std::isalnum(u) || u >= 0x80 ? static_cast<char>(c) : '-';
    }
    return lay_home() / "sessions" / fs::path(std::u8string(slug.begin(), slug.end()));
}

std::string new_session_id() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    std::random_device rd;
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "%04x", rd() & 0xffff);
    return std::string(stamp) + "-" + suffix;
}

loaded_session load_session(const fs::path& path) {
    loaded_session out;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        auto value = json::parse(line, nullptr, false);
        if (value.is_discarded() || !value.is_object()) continue;
        const auto type = value.value("type", "");
        if (type == "message" && value.contains("message")) {
            out.messages.push_back(message_from_json(value["message"]));
        } else if (type == "meta") {
            out.model = value.value("model", out.model);
        } else if (type == "compact" && value.contains("message")) {
            // A compaction replaces everything before it with its summary.
            out.messages.clear();
            out.messages.push_back(message_from_json(value["message"]));
        }
    }
    close_dangling_tool_calls(out.messages);
    return out;
}

std::vector<session_info> list_sessions(const fs::path& workspace) {
    std::vector<session_info> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sessions_dir(workspace), ec)) {
        if (entry.path().extension() != ".jsonl") continue;
        session_info info{entry.path(), entry.path().stem().string(), {}, entry.last_write_time(ec)};
        std::ifstream in(entry.path(), std::ios::binary);
        std::string line;
        for (int i = 0; i < 50 && std::getline(in, line) && info.title.empty(); ++i) {
            auto value = json::parse(line, nullptr, false);
            if (value.is_object() && value.value("type", "") == "message") {
                const auto m = message_from_json(value["message"]);
                if (m.from == message::role::user) info.title = m.plain_text().substr(0, 80);
            }
        }
        out.push_back(std::move(info));
    }
    std::ranges::sort(out, std::greater{}, &session_info::modified);
    return out;
}

void close_dangling_tool_calls(std::vector<message>& messages) {
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].from != message::role::assistant) continue;
        const auto calls = messages[i].tool_uses();
        if (calls.empty()) continue;
        std::set<std::string> answered;
        const bool has_next = i + 1 < messages.size() && messages[i + 1].from == message::role::user;
        if (has_next) {
            for (const auto& b : messages[i + 1].blocks) {
                if (b.type == block::kind::tool_result) answered.insert(b.id);
            }
        }
        std::vector<block> missing;
        for (const auto* c : calls) {
            if (!answered.contains(c->id)) missing.push_back(block::make_tool_result(c->id, "[Interrupted before this tool ran]", true));
        }
        if (missing.empty()) continue;
        if (!has_next) {
            messages.insert(messages.begin() + static_cast<std::ptrdiff_t>(i + 1), message{message::role::user, {}});
        }
        auto& next = messages[i + 1].blocks;
        // Tool results must come first in the user message.
        next.insert(next.begin(), missing.begin(), missing.end());
    }
}

} // namespace lay
