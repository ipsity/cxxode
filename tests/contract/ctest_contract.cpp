#include "tools/ctest.hpp"

#include <stdexcept>
#include <string_view>

int main() {
    using namespace lay;
    const auto require = [](bool value, std::string_view message) {
        if (!value) throw std::runtime_error(std::string(message));
    };
    const auto observed = [](std::string_view bytes, std::int64_t code = 0,
                             ProcessStatus status = ProcessStatus::exited) {
        Verification result;
        result.process.code = code;
        result.process.status = status;
        result.process.stdout_text = {std::string(bytes), {}, 0, bytes.size()};
        observe_ctest(result);
        return result;
    };
    constexpr auto passing =
        "Test project fixture\n"
        "    Start 1: good test\n"
        "1/1 Test #1: good test ........................   Passed    0.07 sec\n\n"
        "100% tests passed out of 1\n";
    constexpr auto failing =
        "Test project fixture\r\n"
        "    Start 2: bad...test\r\n"
        "1/1 Test #2: bad...test .......................***Failed    0.07 sec\r\n\r\n"
        "0% tests passed, 1 tests failed out of 1\r\n";
    constexpr auto nothing = "Test project fixture\nNo tests were found!!!\n";
    constexpr auto skipped =
        "Test project fixture\n"
        "    Start 3: skip test\n"
        "1/1 Test #3: skip test ........................***Skipped   0.07 sec\n\n"
        "100% tests passed out of 1\n"
        "The following tests did not run:\n"
        "   3 - skip test (Skipped)\n";
    constexpr auto disabled =
        "Test project fixture\n"
        "    Start 4: disabled\n"
        "1/1 Test #4: disabled ......................***Not Run (Disabled)   0.00 sec\n"
        "No tests were found!!!\n";

    const auto passed = observed(passing);
    require(passed.test_cases == std::vector<TestCaseEvidence>{{"good test", TestOutcome::passed, 3}} &&
        passed.test_summary == "CTest reported 1 passing test(s).", "passing recording and source line");
    const auto failed = observed(failing, 8);
    require(failed.test_cases == std::vector<TestCaseEvidence>{{"bad...test", TestOutcome::failed, 3}} &&
        failed.test_summary == "CTest reported 1 failed out of 1 test(s).", "failing recording with a dotted name");
    const auto empty = observed(nothing);
    require(empty.test_cases.empty() && empty.test_summary == "CTest reported no tests.", "zero selected tests");
    const auto skip = observed(skipped);
    require(skip.test_cases == std::vector<TestCaseEvidence>{{"skip test", TestOutcome::skipped, 3}} &&
        skip.test_summary == "CTest reported 0 passing, 1 skipped, 0 disabled.", "skipped recording with success exit");
    const auto inactive = observed(disabled);
    require(inactive.test_cases == std::vector<TestCaseEvidence>{{"disabled", TestOutcome::disabled, 3}} &&
        inactive.test_summary == "CTest reported no tests.", "disabled case did not execute");
    const auto cancelled = observed(passing, 0, ProcessStatus::cancelled);
    require(cancelled.test_cases.empty() && cancelled.test_summary.empty(), "cancelled recording");

    Verification truncated;
    truncated.process.status = ProcessStatus::exited;
    truncated.process.stdout_text = {"Test project fixture\n    Start 1: good test\n", "100% tests passed out of 1\n",
                                    4096, 4166};
    observe_ctest(truncated);
    require(truncated.test_cases.empty() && truncated.test_summary == passed.test_summary, "truncation retains summary");

    auto malformed = std::string(passing);
    malformed.replace(malformed.find("good test"), std::string("good test").size(), "different test");
    const auto damaged = observed(malformed);
    require(damaged.test_cases.empty() && damaged.test_summary == passed.test_summary, "inconsistent names retain summary");
    require(observed(passing, 8).test_cases.empty(), "command exit contradicts passing output");

    const auto details = test_case_details(failed);
    require(details.find("[failed] bad...test (stdout line 3)") != details.npos &&
        details.find("captured stdout") != details.npos, "named evidence carries the per-case source");
}
