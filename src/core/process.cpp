#include "core/process.hpp"
#include "core/utf8.hpp"

namespace lay {

std::string quoted_argument(std::string_view value) {
    std::string result(1, '"');
    for (const auto byte : value) {
        if (byte == '"' || byte == '\\') {
            result += '\\';
        }
        if (static_cast<unsigned char>(byte) < 32 || byte == 127) {
            constexpr std::string_view hex = "0123456789abcdef";
            const auto number = static_cast<unsigned char>(byte);
            result += "\\x";
            result += hex[number >> 4];
            result += hex[number & 15];
        } else {
            result += byte;
        }
    }
    result += '"';
    return result;
}

std::string describe(ProcessStatus status, std::int64_t code) {
    switch (status) {
        case ProcessStatus::not_started: return "command could not be started";
        case ProcessStatus::exited: return "command exited " + std::to_string(code);
        case ProcessStatus::signaled: return "command terminated by signal " + std::to_string(code);
        case ProcessStatus::timed_out: return "command timed out";
        case ProcessStatus::cancelled: return "command cancelled";
        case ProcessStatus::unknown: return "command effect or cleanup unknown";
    }
    return "unknown process status";
}

std::string verification_text(const Verification& result) {
    const auto& process = result.process;
    std::string text = describe(process.status, process.code);
    if (!result.test_summary.empty()) {
        text += "\n" + result.test_summary;
    }
    if (!process.diagnostic.empty()) {
        text += "\n" + process.diagnostic;
    }
    if (process.descendants_terminated) {
        text += "\nRemaining descendants were terminated.";
    }
    const auto add = [&](std::string_view name, const CapturedOutput& output) {
        if (output.total_bytes == 0) {
            return;
        }
        constexpr std::size_t limit = 2048;
        text += "\n" + std::string(name) + " (" + std::to_string(output.total_bytes) +
                " bytes, " + std::to_string(output.omitted_bytes) + " omitted during capture):\n";
        text += utf8_prefix(output.head, limit);
        auto tail = std::string_view(output.tail.empty() ? output.head : output.tail);
        if (output.head.size() > limit || !output.tail.empty()) {
            text += "\n[continued in stored output]\n";
            if (tail.size() > limit) {
                auto start = tail.size() - limit;
                while (continuation(static_cast<unsigned char>(tail[start]))) {
                    ++start;
                }
                tail.remove_prefix(start);
            }
            text += tail;
        }
    };
    add("stdout", process.stdout_text);
    add("stderr", process.stderr_text);
    return text;
}

std::string verification_reference(const Verification& result, const RunEvidence& run, std::size_t current_revision) {
    return describe(result.process.status, result.process.code) + " at edit revision " +
        std::to_string(run.edit_revision) + (run.edit_revision < current_revision ? " (earlier workspace revision)" : "");
}

} // namespace lay
