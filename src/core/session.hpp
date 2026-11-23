#pragma once

#include "core/tools.hpp"
#include "core/process.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lay {

enum class ReplyStop { end_turn, tool_use };
enum class SessionStop {
    end_turn, protocol_error, cancelled, request_limit, tool_limit,
    http_error, transport_error, response_limit, refused, time_limit, storage_error, effect_unknown,
    approval_required,
    context_limit,
    repeated_file_failure,
};

struct TextDelta { std::string text; };
struct ProposeTool { ToolCall call; };
struct ProviderItem { std::string json; };
struct FinishReply { ReplyStop reason; };
using ModelEvent = std::variant<TextDelta, ProposeTool, ProviderItem, FinishReply>;

struct EditEvidence {
    std::string path;
    std::string parent;
    std::string before;
    std::string before_digest;
    std::string after_digest;
    std::string temporary;
    std::string backup;
    std::string review_diff{};
    std::optional<bool> approved{};
    bool operator==(const EditEvidence&) const = default;
};

std::string edit_review_details(const EditEvidence& evidence);

struct ToolRecord {
    ToolCall call;
    bool dispatched = false;
    ToolResult result;
    std::size_t text_offset = 0;
    std::optional<EditEvidence> edit{};
    std::optional<RunEvidence> run{};
    std::optional<Verification> verification{};
    bool operator==(const ToolRecord&) const = default;
};

struct ProviderItemPosition {
    std::string json;
    std::size_t text_offset;
    std::size_t tools_before;
    bool operator==(const ProviderItemPosition&) const = default;
};

struct Round {
    std::string text;
    std::vector<ToolRecord> tools;
    std::vector<ProviderItemPosition> provider_items{};
    bool operator==(const Round&) const = default;
};

using UsageFields = std::map<std::string, std::optional<std::size_t>, std::less<>>;

enum class AttemptState { reserved, no_receipt, response };

struct ModelAttempt {
    std::size_t first_round = 0;
    std::size_t retained_rounds = 0;
    std::size_t body_bytes = 0;
    std::size_t budget_bytes = 0;
    std::size_t template_version = 1;
    std::optional<std::size_t> input_tokens;
    std::optional<std::size_t> output_tokens;
    std::optional<std::size_t> first_byte_us{};
    std::optional<std::size_t> request_number{};
    std::optional<std::size_t> uploaded_body_bytes{};
    std::optional<UsageFields> provider_usage{};
    std::optional<AttemptState> state{};
    std::optional<std::size_t> http_status{};
    bool operator==(const ModelAttempt&) const = default;
};

struct TaskSupplement {
    static constexpr std::size_t sequence = 2;
    static constexpr std::size_t max_bytes = 4096;
    std::string text;
    std::int64_t saved_revision;
    std::optional<std::size_t> before_round{};
    bool operator==(const TaskSupplement&) const = default;
};

struct DecisionSource {
    std::size_t task;
    std::string version;
    bool operator==(const DecisionSource&) const = default;
};

struct DecisionReference {
    DecisionSource earlier;
    std::optional<DecisionSource> current{};
    bool operator==(const DecisionReference&) const = default;
};

struct Task {
    std::string question;
    std::size_t first_round = 0;
    std::size_t first_request = 0;
    std::size_t first_dispatch = 0;
    bool allow_edit = true;
    std::optional<SessionStop> ended{};
    std::size_t active_ms = 0;
    std::optional<TaskSupplement> supplement{};
    std::optional<std::size_t> approval_wait_ms{};
    std::optional<DecisionReference> decision_reference{};
    bool operator==(const Task&) const = default;
};

struct Session {
    std::string question;
    std::vector<Round> rounds;
    std::size_t requests = 0;
    std::size_t dispatches = 0;
    SessionStop stop = SessionStop::end_turn;
    std::string diagnostic;
    bool finished = false;
    std::vector<ModelAttempt> model_attempts{};
    std::vector<Task> tasks{};
    bool operator==(const Session&) const = default;
};

struct Request {
    std::string_view question;
    std::span<const Round> history;
    std::size_t attempts_left;
    bool edit_enabled = false;
    bool run_enabled = false;
    std::span<const Task> tasks{};
    std::function<void(const ModelAttempt&)> checkpoint_attempt{};
};

struct GenerationFailure {
    SessionStop reason;
    std::string diagnostic;
};

struct Generation {
    std::size_t attempts = 1;
    std::optional<GenerationFailure> failure;
    std::vector<ModelAttempt> observations{};
};

struct Limits {
    std::size_t requests = 24;
    std::size_t tool_calls = 64;
};

struct TaskReceipt {
    std::string title;
    std::string detail;
};

// The model stops emitting when the sink returns false. Returning without a
// valid FinishReply leaves the draft uncommitted.
using EventSink = std::function<bool(const ModelEvent&)>;
using Generate = std::function<Generation(const Request&, const EventSink&)>;
using ExecuteTool = std::function<ToolResult(const ToolCall&)>;
using AuthorizeTool = std::function<std::string_view(const ToolCall&)>;
using StopRequested = std::function<bool()>;

struct EditPreparation {
    std::optional<EditEvidence> evidence;
    std::string rejection;
    std::function<ToolResult()> apply;
    bool needs_approval = false;
    std::string verified_version{};
    std::optional<FileFailure> file_failure{};
};

struct RunPreparation {
    RunEvidence evidence;
    std::optional<Verification> rejection;
    std::function<Verification()> apply;
    bool needs_approval = false;
};

struct SessionJournal {
    std::function<void(Session&, std::string_view)> save;
    std::function<EditPreparation(const ToolCall&, const Session&)> prepare_edit;
    std::function<ToolResult(const EditEvidence&)> recover_edit;
    std::function<void(const EditEvidence&)> cleanup_edit;
    std::function<RunPreparation(const ToolCall&, const Session&)> prepare_run{};
    bool persist_model_attempts = false;
    std::function<void(Session&)> accept_supplement{};
};

struct TaskActions {
    bool resume = false;
    bool close = false;
    bool follow_up = false;
    bool acknowledge_unknown = false;
};

Session run_session(std::string question, const Generate& generate, const ExecuteTool& execute,
                    const StopRequested& stop_requested, Limits limits = {},
                    const AuthorizeTool& authorize = {});
Session continue_session(Session session, const Generate& generate, const ExecuteTool& execute,
                         const StopRequested& stop_requested, Limits limits,
                         const AuthorizeTool& authorize, const SessionJournal& journal);
void append_task(Session& session, std::string question, bool allow_edit);
TaskActions task_actions(const Session& session);
std::string supplement_id(std::size_t task_id);
std::string supplement_receipt(const TaskSupplement& supplement, std::size_t task_id, bool closed);
std::string task_question_id(std::size_t task_id);
std::string decision_reference_receipt(const DecisionReference& reference);
void close_task(Session& session);
void recover_session(Session& session, const SessionJournal& journal);
bool task_closed(const Session& session);
std::string_view name(SessionStop stop);
std::size_t edit_revision(const Session& session);
TaskReceipt task_receipt(const Session& session, Limits limits = {});

} // namespace lay
