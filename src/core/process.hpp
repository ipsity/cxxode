#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lay {

enum class ProcessStatus { not_started, exited, signaled, timed_out, cancelled, unknown };

struct CapturedOutput {
    std::string head;
    std::string tail;
    std::uint64_t omitted_bytes = 0;
    std::uint64_t total_bytes = 0;
    bool operator==(const CapturedOutput&) const = default;
};

struct ProcessResult {
    ProcessStatus status = ProcessStatus::not_started;
    std::int64_t code = 0;
    std::uint64_t elapsed_ms = 0;
    std::string diagnostic;
    CapturedOutput stdout_text;
    CapturedOutput stderr_text;
    bool descendants_terminated = false;
    bool operator==(const ProcessResult&) const = default;
};

struct RunEvidence {
    std::string program;
    std::vector<std::string> arguments;
    std::string directory;
    std::size_t timeout_ms = 0;
    std::size_t edit_revision = 0;
    bool operator==(const RunEvidence&) const = default;
};

struct Verification {
    ProcessResult process;
    std::string observed_utc;
    std::string test_summary;
    bool operator==(const Verification&) const = default;
};

std::string describe(ProcessStatus status, std::int64_t code);
std::string verification_text(const Verification& result);

} // namespace lay
