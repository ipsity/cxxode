#include "model/scripted_model.hpp"

#include <cstdlib>
#include <iostream>

namespace {

const std::string question = "Can the parser accept an empty configuration?";
const lay::ReadCall expected_call{"read-1", std::string(lay::fixture_path)};

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct Step {
    lay::ModelEvent event;
    bool interrupt;
};

void check(std::string_view label, lay::Script script, lay::Limits limits, bool interrupt,
           std::string fixture, const lay::Session& expected) {
    std::vector<std::vector<Step>> requests;
    std::vector<lay::ReadRecord> observations;
    bool cancelled = false;
    const auto model = lay::scripted_model(script);
    const auto original = lay::run_session(question,
        [&](const lay::Request& request, const lay::EventSink& emit) {
            require(request.question == question, "The model received a different question");
            auto& steps = requests.emplace_back();
            model(request, [&](const lay::ModelEvent& event) {
                cancelled = interrupt && std::holds_alternative<lay::PathDelta>(event);
                steps.push_back({event, cancelled});
                return emit(event);
            });
        },
        [&](const lay::ReadCall& call) {
            require(call == expected_call, "Unexpected fixture dispatch");
            return observations.emplace_back(lay::ReadRecord{call, true,
                {lay::ReadStatus::ok, fixture}}).result;
        }, [&] { return cancelled; }, limits);

    std::cout << label << '\n';
    require(original == expected, "Unexpected committed rounds, terminal state or dispatch count");
    require(observations.size() == expected.dispatches, "Actual dispatch count differs");

    for (int replay = 0; replay < 100; ++replay) {
        std::size_t next_request = 0;
        std::size_t next_observation = 0;
        cancelled = false;
        const auto result = lay::run_session(question,
            [&](const lay::Request&, const lay::EventSink& emit) {
                require(next_request < requests.size(), "Replay requested an unrecorded reply");
                for (const auto& step : requests[next_request++]) {
                    cancelled = step.interrupt;
                    if (!emit(step.event)) {
                        break;
                    }
                }
            },
            [&](const lay::ReadCall& call) {
                require(next_observation < observations.size(), "Replay dispatched an unrecorded call");
                const auto& observation = observations[next_observation++];
                require(observation.call == call, "Replay dispatched a different call");
                return observation.result;
            }, [&] { return cancelled; }, limits);
        require(result == original, "Replay differed from the original session");
        require(next_request == requests.size() && next_observation == observations.size(),
                "Replay did not consume the original trace");
    }
}

} // namespace

int main() {
    using namespace lay;
    const Round successful_read{"", {{expected_call, true, {ReadStatus::ok, "allowed"}}}};
    check("normal", Script::normal, {}, false, "allowed",
        {question, {successful_read, {"config/defaults.txt reports: allowed", {}}},
         2, 1, SessionStop::end_turn});
    check("truncated after one complete call", Script::truncated, {}, false, "",
        {question, {}, 1, 0, SessionStop::protocol_error});
    check("cancelled mid-call", Script::normal, {}, true, "",
        {question, {}, 1, 0, SessionStop::cancelled});
    check("request budget exhausted after read", Script::normal, {1, 64}, false, "allowed",
        {question, {successful_read}, 1, 1, SessionStop::request_limit});

    const Round alternate_read{"", {{expected_call, true, {ReadStatus::ok, "rejected"}}}};
    check("different evidence changes the answer", Script::normal, {}, false, "rejected",
        {question, {alternate_read, {"config/defaults.txt reports: rejected", {}}},
         2, 1, SessionStop::end_turn});
    const Round skipped_read{"", {{expected_call, false, {ReadStatus::skipped, "tool_limit"}}}};
    check("tool budget exhausted before dispatch", Script::normal, {24, 0}, false, "",
        {question, {skipped_read}, 1, 0, SessionStop::tool_limit});
}
