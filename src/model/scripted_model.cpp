#include "model/scripted_model.hpp"

namespace lay {

Generate scripted_model(Script script) {
    return [script](const Request& request, const EventSink& emit) {
        if (request.history.empty()) {
            if (!emit(BeginRead{"read-1"}) || !emit(PathDelta{"config/"}) ||
                !emit(PathDelta{"defaults.txt"}) || !emit(EndRead{})) {
                return;
            }
            if (script == Script::truncated) {
                if (emit(BeginRead{"read-2"})) {
                    emit(PathDelta{"config/over"});
                }
                return;
            }
            emit(FinishReply{ReplyStop::tool_use});
            return;
        }

        const auto& record = request.history.back().reads.front();
        const std::string answer = record.result.status == ReadStatus::ok
            ? record.call.path + " reports: " + record.result.text
            : "The fixture could not be read: " + record.result.text;
        if (emit(TextDelta{answer})) {
            emit(FinishReply{ReplyStop::end_turn});
        }
    };
}

} // namespace lay
