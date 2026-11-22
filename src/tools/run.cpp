#include "tools/run.hpp"
#include "core/utf8.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace lay {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view denied = "command denied by user";

std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}

std::string lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    return value;
}

struct Stamp {
    std::uintmax_t size;
    fs::file_time_type modified;
    bool operator==(const Stamp&) const = default;
};

std::optional<Stamp> executable_stamp(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(path, error))) {
        return std::nullopt;
    }
#ifdef _WIN32
    const auto root = path.root_name().native();
    if (root.size() != 2 || root[1] != L':' || lower(utf8(path.extension())) != ".exe") {
        return std::nullopt;
    }
#else
    if (::access(path.c_str(), X_OK) != 0) {
        return std::nullopt;
    }
#endif
    const auto size = fs::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    const auto modified = fs::last_write_time(path, error);
    return error ? std::nullopt : std::optional(Stamp{size, modified});
}

fs::path resolved_executable(const fs::path& candidate) {
    std::error_code error;
    auto resolved = fs::canonical(candidate, error);
    return !error && executable_stamp(resolved) ? resolved : fs::path{};
}

fs::path find_on_path(const fs::path& basename, const TextList& environment) {
#ifdef _WIN32
    constexpr char separator = ';';
    auto filename = basename;
    if (!filename.has_extension()) {
        filename += L".exe";
    }
#else
    constexpr char separator = ':';
    const auto& filename = basename;
#endif
    const auto entry = std::ranges::find_if(environment, [](const auto& line) { return line.starts_with("PATH="); });
    if (entry == environment.end()) {
        return {};
    }
    auto path = std::string_view(*entry).substr(5);
    while (!path.empty()) {
        const auto split = path.find(separator);
        auto part = path.substr(0, split);
#ifdef _WIN32
        if (part.size() >= 2 && part.front() == '"' && part.back() == '"') {
            part = part.substr(1, part.size() - 2);
        }
#endif
        const auto directory = platform::path_from_utf8(part);
        if (directory.is_absolute()) {
            auto resolved = resolved_executable(directory / filename);
            if (!resolved.empty()) {
                return resolved;
            }
        }
        if (split == path.npos) {
            break;
        }
        path.remove_prefix(split + 1);
    }
    return {};
}

std::string utc() {
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

std::optional<std::pair<unsigned int, unsigned int>> ctest_counts(std::string_view text) {
    constexpr std::string_view marker = "% tests passed, ";
    constexpr std::string_view all_passed = "% tests passed out of ";
    std::optional<std::pair<unsigned int, unsigned int>> counts;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        const auto first = line.find_first_not_of(" \t");
        if (first != line.npos) {
            line.remove_prefix(first);
        }
        unsigned percentage = 0, failed = 0, total = 0;
        const auto prefix = std::from_chars(line.data(), line.data() + line.size(), percentage);
        auto rest = line.substr(static_cast<std::size_t>(prefix.ptr - line.data()));
        if (prefix.ec == std::errc{} && percentage == 100 && rest.starts_with(all_passed)) {
            rest.remove_prefix(all_passed.size());
            const auto limit = std::from_chars(rest.data(), rest.data() + rest.size(), total);
            if (limit.ec == std::errc{} && limit.ptr == rest.data() + rest.size() && total) {
                counts = std::pair(0u, total);
            }
        } else if (prefix.ec == std::errc{} && percentage <= 100 && rest.starts_with(marker)) {
            rest.remove_prefix(marker.size());
            const auto count = std::from_chars(rest.data(), rest.data() + rest.size(), failed);
            rest.remove_prefix(static_cast<std::size_t>(count.ptr - rest.data()));
            constexpr std::string_view middle = " tests failed out of ";
            if (count.ec == std::errc{} && rest.starts_with(middle)) {
                rest.remove_prefix(middle.size());
                const auto limit = std::from_chars(rest.data(), rest.data() + rest.size(), total);
                if (limit.ec == std::errc{} && limit.ptr == rest.data() + rest.size() && total && failed <= total) {
                    counts = std::pair(failed, total);
                }
            }
        }
        if (end == text.npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return counts;
}

std::string test_summary(const fs::path& executable, const ProcessResult& result) {
    const auto basename = lower(utf8(executable.filename()));
    if ((basename != "ctest" && basename != "ctest.exe") ||
        result.status != ProcessStatus::exited || result.descendants_terminated) {
        return {};
    }
    const auto& out = result.stdout_text;
    const auto& err = result.stderr_text;
    for (const auto* captured : {&out, &err}) {
        const auto counts = captured->omitted_bytes == 0
            ? ctest_counts(captured->head + captured->tail)
            : ctest_counts(captured->tail);
        if (counts) {
            const auto [failed, total] = *counts;
            if (failed == 0 && result.code == 0) {
                return "CTest reported " + std::to_string(total) + " passing test(s).";
            }
            if (failed != 0 && result.code != 0) {
                return "CTest reported " + std::to_string(failed) + " failed out of " + std::to_string(total) + " test(s).";
            }
        }
        if (captured->head.find("No tests were found") != std::string::npos ||
            captured->tail.find("No tests were found") != std::string::npos) {
            return "CTest reported no tests.";
        }
    }
    return {};
}

RunPreparation reject(RunEvidence evidence, std::string detail) {
    Verification result;
    result.process.diagnostic = std::move(detail);
    result.observed_utc = utc();
    return {std::move(evidence), std::move(result), {}, false};
}

bool denied_before(const Session& session, const RunEvidence& evidence) {
    for (const auto& round : session.rounds) {
        for (const auto& tool : round.tools) {
            if (tool.run == evidence && tool.verification &&
                tool.verification->process.diagnostic == denied) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

RunPreparation RunWorkspace::prepare(const ToolCall& call, const Session& session,
                                     const ReviewRun& review, const StopRequested& stop) const {
    RunEvidence evidence{std::string(text_arg(call, "program")), list_arg(call, "args"),
                         std::string(text_arg(call, "path")), number_arg(call, "timeout_ms", 30000),
                         edit_revision(session)};
    if (evidence.directory.empty()) {
        evidence.directory = ".";
    }
    const auto relative_directory = evidence.directory;
    if (!workspace_.allowed(relative_directory)) {
        return reject(std::move(evidence), "Working directory is outside the readable workspace scope");
    }
    const auto open_directory = [this, relative_directory] {
        return relative_directory == "." ? workspace_.root_.duplicate() : workspace_.open(relative_directory);
    };
    try {
        auto directory = open_directory();
        if (!directory || !directory.directory()) {
            return reject(std::move(evidence), "Working directory is missing or unsupported");
        }
        evidence.directory = utf8(directory.current_path());
        const auto directory_id = directory.identity();
        auto environment = platform::child_environment();
        auto proposed = std::string_view(evidence.program);
        const auto path = platform::path_from_utf8(proposed);
        fs::path executable;
        if (path.is_absolute()) {
            executable = resolved_executable(path);
        } else if (!path.has_root_path() && path.has_parent_path()) {
            if (proposed.starts_with("./")) {
                proposed.remove_prefix(2);
            }
            if (workspace_.allowed(proposed)) {
                const auto file = workspace_.open(proposed);
                if (file && !file.directory()) {
                    executable = resolved_executable(workspace_.root_.current_path() / platform::path_from_utf8(proposed));
                }
            }
        } else if (!path.has_root_path() && proposed != "." && proposed != "..") {
            executable = find_on_path(path, environment);
        }
        const auto stamp = executable.empty() ? std::nullopt : executable_stamp(executable);
        if (!stamp) {
            return reject(std::move(evidence), "Executable was not found or cannot be launched directly");
        }
        evidence.program = utf8(executable);
        if (denied_before(session, evidence)) {
            return reject(std::move(evidence), std::string(denied));
        }
        const auto approval = review(evidence);
        if (approval == Approval::unavailable || stop()) {
            return {std::move(evidence), {}, {}, true};
        }
        if (approval == Approval::denied) {
            return reject(std::move(evidence), std::string(denied));
        }
        directory = open_directory();
        const auto current = [&] {
            return directory && directory.identity() == directory_id &&
                utf8(directory.current_path()) == evidence.directory && executable_stamp(executable) == stamp;
        };
        if (!current()) {
            return reject(std::move(evidence), "Executable or working directory changed during approval");
        }
        const auto apply = [evidence, executable, stamp, directory_id, open_directory,
                            environment = std::move(environment), stop]() -> Verification {
            ProcessResult result;
            bool starting = false;
            try {
                auto directory = open_directory();
                if (stop()) {
                    result.status = ProcessStatus::cancelled;
                } else if (!directory || directory.identity() != directory_id ||
                           utf8(directory.current_path()) != evidence.directory ||
                           executable_stamp(executable) != stamp) {
                    result.diagnostic = "Executable or working directory changed before launch";
                } else {
                    starting = true;
                    result = platform::run_process({executable, directory, evidence.arguments, environment,
                        std::chrono::milliseconds(evidence.timeout_ms)}, stop);
                }
            } catch (const std::runtime_error& error) {
                result.status = starting ? ProcessStatus::unknown : ProcessStatus::not_started;
                result.diagnostic = error.what();
            }
            const auto summary = test_summary(executable, result);
            return {std::move(result), utc(), summary};
        };
        return {std::move(evidence), {}, apply, false};
    } catch (const std::runtime_error& error) {
        return reject(std::move(evidence), error.what());
    }
}

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

} // namespace lay
