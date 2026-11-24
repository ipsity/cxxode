#include "model/scripted_model.hpp"

#include <cstdlib>
#include <iostream>

namespace {

const std::string question = "Can the parser accept an empty configuration?";
const lay::ToolCall expected_call{"read-1", "read", {{"path", std::string(lay::fixture_path)}}};

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
    std::vector<lay::ToolRecord> observations;
    bool cancelled = false;
    const auto model = lay::scripted_model(script);
    const auto original = lay::run_session(question,
        [&](const lay::Request& request, const lay::EventSink& emit) {
            require(request.question == question, "The model received a different question");
            auto& steps = requests.emplace_back();
            return model(request, [&](const lay::ModelEvent& event) {
                cancelled = interrupt && std::holds_alternative<lay::ProposeTool>(event);
                steps.push_back({event, cancelled});
                return emit(event);
            });
        },
        [&](const lay::ToolCall& call) {
            require(call == expected_call, "Unexpected fixture dispatch");
            return observations.emplace_back(lay::ToolRecord{call, true,
                {lay::ToolStatus::ok, fixture}}).result;
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
                return lay::Generation{};
            },
            [&](const lay::ToolCall& call) {
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
    const Round successful_read{"", {{expected_call, true, {ToolStatus::ok, "allowed"}}}};
    check("normal", Script::normal, {}, false, "allowed",
        {question, {successful_read, {"config/defaults.txt reports: allowed", {}}},
         2, 1, SessionStop::end_turn, {}, true});
    check("truncated after one complete call", Script::truncated, {}, false, "",
        {question, {}, 1, 0, SessionStop::protocol_error, {}});
    check("cancelled before the reply commits", Script::normal, {}, true, "",
        {question, {}, 1, 0, SessionStop::cancelled, {}});
    check("request budget exhausted after read", Script::normal, {1, 64}, false, "allowed",
        {question, {successful_read}, 1, 1, SessionStop::request_limit, {}});

    const Round alternate_read{"", {{expected_call, true, {ToolStatus::ok, "rejected"}}}};
    check("different evidence changes the answer", Script::normal, {}, false, "rejected",
        {question, {alternate_read, {"config/defaults.txt reports: rejected", {}}},
         2, 1, SessionStop::end_turn, {}, true});
    const Round skipped_read{"", {{expected_call, false, {ToolStatus::skipped, "tool_limit"}}}};
    check("tool budget exhausted before dispatch", Script::normal, {24, 0}, false, "",
        {question, {skipped_read}, 1, 0, SessionStop::tool_limit, {}});

    auto exhausted = run_session(question, scripted_model(Script::normal),
        [](const ToolCall&) { return ToolResult{ToolStatus::ok, "allowed"}; }, [] { return false; }, {1, 1});
    exhausted.tasks.push_back({question});
    const std::string follow_up = "Check again using the new task's budget.";
    append_task(exhausted, follow_up, false);
    const auto continued = continue_session(std::move(exhausted),
        [&](const Request& request, const EventSink& emit) {
            require(request.question == follow_up && request.attempts_left == 1 && request.tasks.size() == 2,
                    "The follow-up sees its own instruction and a fresh request allowance");
            emit(ProposeTool{expected_call});
            emit(FinishReply{ReplyStop::tool_use});
            return Generation{};
        }, [&](const ToolCall&) { return ToolResult{ToolStatus::ok, "new observation"}; },
        [] { return false; }, {1, 1}, {}, {});
    require(continued.tasks.front().ended == SessionStop::request_limit && continued.requests == 2 &&
            continued.dispatches == 2 && continued.rounds.back().tools.front().result.text == "new observation",
            "The previous task's dispatch count does not consume the follow-up allowance");

    Session limited;
    limited.question = question;
    limited.tasks.push_back({question});
    const auto no_dispatch = [](const ToolCall&) -> ToolResult { throw std::runtime_error("Unexpected dispatch"); };
    auto paused = continue_session(std::move(limited),
        [](const Request&, const EventSink&) {
            return Generation{0, GenerationFailure{SessionStop::context_limit, "Context is full before sending"}};
        }, no_dispatch, [] { return false; }, {}, {}, {});
    auto actions = task_actions(paused);
    require(actions.resume && actions.close && !actions.follow_up && paused.requests == 0,
            "A local context limit offers an offline exit without a model attempt");
    close_task(paused);
    require(task_actions(paused).follow_up && paused.requests == 0, "Closing the task does not request the model");
    append_task(paused, follow_up, false);
    require(paused.tasks.front().ended == SessionStop::context_limit && paused.tasks.back().first_request == 0,
            "The next task keeps the original stop reason and starts with a fresh budget");
}
