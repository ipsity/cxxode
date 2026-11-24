#include "tools/ctest.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <optional>
#include <utility>

namespace lay {
namespace {

struct Counts {
    unsigned failed;
    unsigned total;
};

std::string_view trim_left(std::string_view line) {
    const auto start = line.find_first_not_of(" \t");
    return start == line.npos ? line.substr(line.size()) : line.substr(start);
}

bool take(std::string_view& line, std::string_view prefix) {
    if (!line.starts_with(prefix)) return false;
    line.remove_prefix(prefix.size());
    return true;
}

bool number(std::string_view& line, unsigned& count) {
    const auto parsed = std::from_chars(line.data(), line.data() + line.size(), count);
    if (parsed.ec != std::errc{}) return false;
    line.remove_prefix(static_cast<std::size_t>(parsed.ptr - line.data()));
    return true;
}

template <typename Visit>
bool lines(std::string_view text, const Visit& visit) {
    std::size_t ordinal = 0;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (!visit(trim_left(line), ++ordinal)) return false;
        if (end == text.npos) break;
        text.remove_prefix(end + 1);
    }
    return true;
}

std::optional<Counts> summary_line(std::string_view line) {
    unsigned percent = 0, failed = 0, total = 0;
    if (!number(line, percent) || percent > 100) return {};
    if (take(line, "% tests passed, ")) {
        if (!number(line, failed) || !take(line, " tests failed out of ")) return {};
    } else if (percent != 100 || !take(line, "% tests passed out of ")) {
        return {};
    }
    if (!number(line, total) || !line.empty() || !total || failed > total) return {};
    return Counts{failed, total};
}

std::optional<Counts> ctest_counts(std::string_view text) {
    std::optional<Counts> counts;
    lines(text, [&](std::string_view line, std::size_t) {
        if (const auto summary = summary_line(line)) counts = summary;
        return true;
    });
    return counts;
}

std::optional<TestOutcome> outcome(std::string_view status) {
    const auto start = status.find_first_not_of(". \t");
    if (start == 0 || start == status.npos) return {};
    status.remove_prefix(start);
    constexpr std::pair<std::string_view, TestOutcome> labels[] = {
        {"Passed", TestOutcome::passed},
        {"***Failed", TestOutcome::failed},
        {"***Timeout", TestOutcome::failed},
        {"***Exception: SegFault", TestOutcome::failed},
        {"***Skipped", TestOutcome::skipped},
        {"***Not Run (Disabled)", TestOutcome::disabled},
        {"***Not Run", TestOutcome::not_run},
    };
    for (const auto& [label, state] : labels) {
        if (status.starts_with(label) && status.size() > label.size() && status[label.size()] == ' ') {
            auto duration = trim_left(status.substr(label.size()));
            if (!duration.ends_with(" sec")) return {};
            duration.remove_suffix(4);
            double seconds = 0;
            const auto parsed = std::from_chars(duration.data(), duration.data() + duration.size(),
                                                seconds, std::chars_format::fixed);
            if (parsed.ec == std::errc{} && parsed.ptr == duration.data() + duration.size() && seconds >= 0) return state;
            return {};
        }
    }
    return {};
}

std::vector<TestCaseEvidence> cases(std::string_view text, bool no_tests, std::int64_t exit_code) {
    constexpr unsigned max_cases = 512;
    std::map<unsigned, std::string_view> started;
    std::vector<TestCaseEvidence> results;
    std::optional<Counts> counts;
    unsigned progress_total = 0;
    bool header = false;
    const auto complete = lines(text, [&](std::string_view line, std::size_t source_line) {
        if (line.starts_with("Test project ")) {
            if (header || !started.empty() || !results.empty()) return false;
            header = true;
            return true;
        }
        if (const auto summary = summary_line(line)) {
            if (counts || !header) return false;
            counts = summary;
            return true;
        }
        if (take(line, "Start ")) {
            unsigned id = 0;
            line = trim_left(line);
            return header && !counts && number(line, id) && id && take(line, ": ") && !line.empty() &&
                line.size() <= 2048 && started.size() < max_cases && started.try_emplace(id, line).second;
        }
        unsigned progress = 0, total = 0, id = 0;
        if (!number(line, progress) || !take(line, "/") || !number(line, total) ||
            !take(line, " Test #")) return true;
        line = trim_left(line);
        if (!header || counts || !number(line, id) || !take(line, ": ") ||
            progress != results.size() + 1 || !total || total > max_cases || progress > total ||
            (progress_total && total != progress_total)) return false;
        const auto previous = started.find(id);
        if (previous == started.end() || previous->second.empty() || !take(line, previous->second)) return false;
        const auto state = outcome(line);
        if (!state) return false;
        results.push_back({std::string(previous->second), *state, source_line});
        previous->second = {}; // Retain the ID to reject a second Start for the same case.
        progress_total = total;
        return true;
    });
    if (!complete || results.empty() || results.size() != progress_total || results.size() != started.size()) return {};
    if (std::ranges::any_of(started, [](const auto& item) { return !item.second.empty(); })) return {};

    const auto disabled = std::ranges::count(results, TestOutcome::disabled, &TestCaseEvidence::outcome);
    const auto failed = std::ranges::count(results, TestOutcome::failed, &TestCaseEvidence::outcome) +
        std::ranges::count(results, TestOutcome::not_run, &TestCaseEvidence::outcome);
    if (counts) {
        if (counts->total == results.size() - static_cast<std::size_t>(disabled) &&
            counts->failed == static_cast<unsigned>(failed) &&
            (failed == 0) == (exit_code == 0)) return results;
    } else if (no_tests && static_cast<std::size_t>(disabled) == results.size()) {
        return results;
    }
    return {};
}

} // namespace

void observe_ctest(Verification& evidence) {
    const auto& result = evidence.process;
    if (result.status != ProcessStatus::exited || result.descendants_terminated) return;
    const auto& out = result.stdout_text;
    const auto& err = result.stderr_text;
    constexpr std::string_view empty = "No tests were found";
    const auto contains = [](const CapturedOutput& output, std::string_view part) {
        return output.head.find(part) != std::string::npos || output.tail.find(part) != std::string::npos;
    };
    if (!out.omitted_bytes && !err.omitted_bytes) {
        evidence.test_cases = cases(out.head + out.tail, contains(out, empty) || contains(err, empty), result.code);
    }
    for (const auto* captured : {&out, &err}) {
        const auto counts = captured->omitted_bytes == 0 ? ctest_counts(captured->head + captured->tail) :
                                                           ctest_counts(captured->tail);
        if (counts) {
            const auto [failed, total] = *counts;
            if (failed == 0 && result.code == 0) {
                const auto skipped = std::ranges::count(evidence.test_cases, TestOutcome::skipped, &TestCaseEvidence::outcome);
                const auto disabled = std::ranges::count(evidence.test_cases, TestOutcome::disabled, &TestCaseEvidence::outcome);
                if (skipped || disabled) {
                    const auto passed = std::ranges::count(evidence.test_cases, TestOutcome::passed, &TestCaseEvidence::outcome);
                    evidence.test_summary = "CTest reported " + std::to_string(passed) + " passing, " +
                        std::to_string(skipped) + " skipped, " + std::to_string(disabled) + " disabled.";
                } else if (contains(*captured, "The following tests did not run:")) {
                    evidence.test_summary = "CTest reported no failures out of " + std::to_string(total) + " test(s).";
                } else {
                    evidence.test_summary = "CTest reported " + std::to_string(total) + " passing test(s).";
                }
                return;
            }
            if (failed != 0 && result.code != 0) {
                evidence.test_summary = "CTest reported " + std::to_string(failed) + " failed out of " +
                    std::to_string(total) + " test(s).";
                return;
            }
        }
        if (contains(*captured, empty)) {
            evidence.test_summary = "CTest reported no tests.";
            return;
        }
    }
}

} // namespace lay
