#include "model/scripted_model.hpp"

namespace lay {

Generate scripted_model(Script script) {
    return [script](const Request& request, const EventSink& emit) -> Generation {
        if (request.history.empty()) {
            const ModelEvent proposal = ProposeTool{{"read-1", "read", {{"path", std::string(fixture_path)}}}};
            if (emit(proposal) && script == Script::normal) {
                emit(FinishReply{ReplyStop::tool_use});
            }
            return {};
        }

        const auto& record = request.history.back().tools.front();
        const std::string answer = record.result.status == ToolStatus::ok
            ? std::string(text_arg(record.call, "path")) + " reports: " + record.result.text
            : "The fixture could not be read: " + record.result.text;
        if (emit(TextDelta{answer})) {
            emit(FinishReply{ReplyStop::end_turn});
        }
        return {};
    };
}

} // namespace lay
