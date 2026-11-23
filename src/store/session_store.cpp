#include "store/session_store.hpp"
#include "core/utf8.hpp"
#include "platform/crypto.hpp"
#include "platform/workspace.hpp"
#ifdef LAY_BENCHMARK_METRICS
#include "core/benchmark.hpp"
#endif

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lay {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr auto current_format = SessionStore::format_version;

bool valid_supplement(std::string_view text) {
    return text.size() <= TaskSupplement::max_bytes && text.find_first_not_of(" \t\r\n") != text.npos &&
        text.find('\0') == text.npos && valid_utf8(text);
}

std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}

fs::path session_directory(const fs::path& root) {
#ifdef _WIN32
    const auto* base = _wgetenv(L"LOCALAPPDATA");
    if (!base || !*base) {
        throw std::runtime_error("LOCALAPPDATA is unavailable");
    }
    const auto directory = fs::path(base) / "LayCode" / "sessions";
#else
    const auto* state = std::getenv("XDG_STATE_HOME");
    const auto* home = std::getenv("HOME");
    if ((!state || !*state) && (!home || !*home)) {
        throw std::runtime_error("the private state directory is unavailable");
    }
    const auto base = state && *state ? fs::path(state) : fs::path(home) / ".local" / "state";
    const auto directory = base / "lay-code" / "sessions";
#endif
    if (!directory.is_absolute()) {
        throw std::runtime_error("the private state directory must be absolute");
    }
    const auto canonical = fs::weakly_canonical(directory);
    const auto workspace = root.empty() ? fs::path{} : fs::canonical(root);
    const auto outside = [&](const fs::path& candidate) {
        return workspace.empty() || std::ranges::mismatch(candidate, workspace, [](const auto& a, const auto& b) {
            return platform::path_equal(utf8(a), utf8(b));
        }).in2 != workspace.end();
    };
    if (!outside(canonical)) {
        throw std::runtime_error("the session directory must be outside the workspace");
    }
    fs::create_directories(canonical);
    const auto actual = fs::canonical(canonical);
    if (!outside(actual)) {
        throw std::runtime_error("the session directory must be outside the workspace");
    }
#ifdef _WIN32
    // SQLite's NT handle semantics and the writer lock are supported on local NTFS.
    const auto checked = platform::Node::root(actual);
    (void)checked;
#else
    struct stat info{};
    if (::stat(actual.c_str(), &info) != 0 || info.st_uid != ::geteuid() ||
        ::chmod(actual.c_str(), 0700) != 0) {
        throw std::runtime_error("the session directory could not be made private");
    }
#endif
    return actual;
}

class WriterLock {
public:
    explicit WriterLock(const fs::path& path) {
#ifdef _WIN32
        const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_SHARING_VIOLATION) {
                throw std::runtime_error("this session already has an active writer");
            }
            throw std::runtime_error("could not open the session writer lock");
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info) || info.nNumberOfLinks != 1 ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            CloseHandle(file);
            throw std::runtime_error("unsupported session writer lock file");
        }
        file_ = file;
#else
        const auto file = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (file == -1) {
            throw std::runtime_error("could not open the session writer lock");
        }
        struct stat info{};
        if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
            info.st_uid != ::geteuid()) {
            ::close(file);
            throw std::runtime_error("unsupported session writer lock file");
        }
        if (::flock(file, LOCK_EX | LOCK_NB) != 0) {
            const auto error = errno;
            ::close(file);
            if (error == EWOULDBLOCK) {
                throw std::runtime_error("this session already has an active writer");
            }
            throw std::runtime_error("could not lock the session for writing");
        }
        file_ = file;
#endif
    }

    ~WriterLock() {
#ifdef _WIN32
        CloseHandle(file_);
#else
        ::close(file_);
#endif
    }

    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;

private:
#ifdef _WIN32
    HANDLE file_;
#else
    int file_;
#endif
};

void check(sqlite3* db, int code, int expected = SQLITE_OK) {
    if (code != expected) {
        throw std::runtime_error("SQLite: " + std::string(db ? sqlite3_errmsg(db) : sqlite3_errstr(code)) +
                                 " (code " + std::to_string(code) + ")");
    }
}

void sql(sqlite3* db, const char* query) {
    check(db, sqlite3_exec(db, query, nullptr, nullptr, nullptr));
}

struct Transaction {
    explicit Transaction(sqlite3* db) : db(db) { sql(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() {
        sql(db, "COMMIT");
        committed = true;
    }
    sqlite3* db;
    bool committed = false;
};

class Statement {
public:
    Statement(sqlite3* db, const char* query) : db_(db) {
        check(db_, sqlite3_prepare_v2(db_, query, -1, &statement_, nullptr));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int index, std::string_view text) {
        check(db_, sqlite3_bind_text(statement_, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT));
    }
    void bind(int index, std::int64_t number) {
        check(db_, sqlite3_bind_int64(statement_, index, number));
    }
    void step(int expected) { check(db_, sqlite3_step(statement_), expected); }
    std::int64_t integer(int index) { return sqlite3_column_int64(statement_, index); }
    std::string text(int index) {
        const auto* value = sqlite3_column_text(statement_, index);
        return value ? std::string(reinterpret_cast<const char*>(value), sqlite3_column_bytes(statement_, index))
                     : std::string{};
    }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

std::string scope_record(const SessionScope& scope) {
    // Keep the existing scope encoding for read-only and single-file sessions.
    Json write = "";
    if (scope.writable_paths.size() == 1) write = scope.writable_paths.front();
    if (scope.writable_paths.size() > 1) write = scope.writable_paths;
    Json result{{"root", utf8(fs::canonical(scope.root))}, {"identity", scope.root_identity},
                {"provider", scope.provider}, {"model", scope.model}, {"write", std::move(write)},
                {"excluded", scope.excluded}};
    if (scope.allow_run) {
        result["allow_run"] = true;
    }
    return result.dump();
}

Json encode_edit(const EditEvidence& edit) {
    Json value{{"path", edit.path}, {"parent", edit.parent}, {"before", edit.before},
            {"before_digest", edit.before_digest}, {"after_digest", edit.after_digest},
            {"temporary", edit.temporary}, {"backup", edit.backup}};
    if (!edit.review_diff.empty()) value["diff"] = edit.review_diff;
    if (edit.approved) value["approved"] = *edit.approved;
    return value;
}

EditEvidence decode_edit(const Json& value) {
    EditEvidence edit{value.at("path"), value.at("parent"), value.at("before"), value.at("before_digest"),
                      value.at("after_digest"), value.at("temporary"), value.at("backup"),
                      value.value("diff", std::string{})};
    if (value.contains("approved")) edit.approved = value.at("approved").get<bool>();
    return edit;
}

Json encode_run(const RunEvidence& run) {
    return {{"program", run.program}, {"arguments", run.arguments}, {"directory", run.directory},
            {"timeout_ms", run.timeout_ms}, {"edit_revision", run.edit_revision}};
}

RunEvidence decode_run(const Json& run) {
    return {run.at("program").get<std::string>(), run.at("arguments").get<TextList>(),
            run.at("directory").get<std::string>(), run.at("timeout_ms").get<std::size_t>(),
            run.at("edit_revision").get<std::size_t>()};
}

Json encode_output(const CapturedOutput& out) {
    return {{"head", out.head}, {"tail", out.tail}, {"omitted_bytes", out.omitted_bytes},
            {"total_bytes", out.total_bytes}};
}

CapturedOutput decode_output(const Json& out) {
    return {out.at("head"), out.at("tail"), out.at("omitted_bytes"), out.at("total_bytes")};
}

Json encode_verification(const Verification& value) {
    const auto& process = value.process;
    Json encoded{{"status", static_cast<int>(process.status)}, {"code", process.code},
            {"elapsed_ms", process.elapsed_ms}, {"diagnostic", process.diagnostic},
            {"stdout", encode_output(process.stdout_text)}, {"stderr", encode_output(process.stderr_text)},
            {"descendants_terminated", process.descendants_terminated},
            {"observed_utc", value.observed_utc}, {"test_summary", value.test_summary}};
    if (!value.test_cases.empty()) {
        auto cases = Json::array();
        for (const auto& test : value.test_cases) {
            cases.push_back({{"name", test.name}, {"outcome", static_cast<int>(test.outcome)},
                             {"stdout_line", test.stdout_line}});
        }
        encoded["test_cases"] = std::move(cases);
    }
    return encoded;
}

Verification decode_verification(const Json& value) {
    const auto status = value.at("status").get<int>();
    if (status < 0 || status > static_cast<int>(ProcessStatus::unknown)) {
        throw std::runtime_error("invalid stored process status");
    }
    Verification verification{{static_cast<ProcessStatus>(status), value.at("code"), value.at("elapsed_ms"),
             value.at("diagnostic"), decode_output(value.at("stdout")), decode_output(value.at("stderr")),
             value.at("descendants_terminated")}, value.at("observed_utc"), value.at("test_summary")};
    if (value.contains("test_cases")) {
        const auto& cases = value.at("test_cases");
        if (!cases.is_array() || cases.size() > 512) throw std::runtime_error("invalid stored test cases");
        for (const auto& test : cases) {
            const auto outcome = test.at("outcome").get<int>();
            if (outcome < 0 || outcome > static_cast<int>(TestOutcome::not_run)) {
                throw std::runtime_error("invalid stored test outcome");
            }
            verification.test_cases.push_back({test.at("name"), static_cast<TestOutcome>(outcome), test.at("stdout_line")});
        }
    }
    return verification;
}

Json encode_attempt(const ModelAttempt& attempt) {
    Json encoded{
        {"first_round", attempt.first_round}, {"retained_rounds", attempt.retained_rounds},
        {"body_bytes", attempt.body_bytes}, {"budget_bytes", attempt.budget_bytes},
        {"template_version", attempt.template_version},
        {"input_tokens", attempt.input_tokens ? Json(*attempt.input_tokens) : Json(nullptr)},
        {"output_tokens", attempt.output_tokens ? Json(*attempt.output_tokens) : Json(nullptr)},
        {"first_byte_us", attempt.first_byte_us ? Json(*attempt.first_byte_us) : Json(nullptr)},
    };
    if (attempt.request_number) encoded["request_number"] = *attempt.request_number;
    if (attempt.uploaded_body_bytes) encoded["uploaded_body_bytes"] = *attempt.uploaded_body_bytes;
    if (attempt.state) encoded["attempt_state"] = static_cast<int>(*attempt.state);
    if (attempt.http_status) encoded["http_status"] = *attempt.http_status;
    if (attempt.provider_usage) {
        auto reported = Json::object();
        for (const auto& [field, count] : *attempt.provider_usage) {
            reported[field] = count ? Json(*count) : Json(nullptr);
        }
        encoded["provider_usage"] = std::move(reported);
    }
    return encoded;
}

ModelAttempt decode_attempt(const Json& value) {
    const auto count = [&](std::string_view name) -> std::optional<std::size_t> {
        const auto& field = value.at(name);
        return field.is_null() ? std::nullopt : std::optional(field.get<std::size_t>());
    };
    ModelAttempt attempt{value.at("first_round"), value.at("retained_rounds"), value.at("body_bytes"),
            value.at("budget_bytes"), value.at("template_version"), count("input_tokens"), count("output_tokens"),
            value.contains("first_byte_us") ? count("first_byte_us") : std::nullopt};
    if (value.contains("request_number")) attempt.request_number = count("request_number");
    if (value.contains("uploaded_body_bytes")) attempt.uploaded_body_bytes = count("uploaded_body_bytes");
    if (value.contains("attempt_state")) {
        const auto state = value.at("attempt_state").get<int>();
        if (state < 0 || state > static_cast<int>(AttemptState::response)) {
            throw std::runtime_error("invalid stored model attempt state");
        }
        attempt.state = static_cast<AttemptState>(state);
    }
    if (value.contains("http_status")) attempt.http_status = count("http_status");
    if (const auto reported = value.find("provider_usage"); reported != value.end()) {
        if (!reported->is_object()) throw std::runtime_error("invalid stored provider usage");
        attempt.provider_usage.emplace();
        for (const auto& [field, tokens] : reported->items()) {
            attempt.provider_usage->emplace(field, tokens.is_null() ? std::nullopt : std::optional(tokens.get<std::size_t>()));
        }
    }
    return attempt;
}

std::string encode_session(const Session& session) {
    auto rounds = Json::array();
    for (const auto& round : session.rounds) {
        auto tools = Json::array();
        for (const auto& record : round.tools) {
            auto args = Json::array();
            for (const auto& arg : record.call.arguments) {
                std::visit([&](const auto& value) { args.push_back(Json::array({arg.name, arg.value.index(), value})); },
                           arg.value);
            }
            tools.push_back({
                {"id", record.call.id}, {"name", record.call.name}, {"arguments", std::move(args)},
                {"dispatched", record.dispatched}, {"offset", record.text_offset},
                {"status", static_cast<int>(record.result.status)}, {"result", record.result.text},
                {"version", record.result.version}, {"edit", record.edit ? encode_edit(*record.edit) : Json(nullptr)},
            });
            if (record.run) {
                tools.back()["run"] = encode_run(*record.run);
            }
            if (!record.result.observed_utc.empty()) {
                tools.back()["observed_utc"] = record.result.observed_utc;
            }
            if (record.result.file_failure) {
                tools.back()["file_failure"] = static_cast<int>(*record.result.file_failure);
            }
            if (record.verification) {
                tools.back()["verification"] = encode_verification(*record.verification);
            }
        }
        rounds.push_back({{"text", round.text}, {"tools", std::move(tools)}});
        if (!round.provider_items.empty()) {
            auto items = Json::array();
            for (const auto& item : round.provider_items) {
                items.push_back({{"json", item.json}, {"text_offset", item.text_offset},
                                 {"tools_before", item.tools_before}});
            }
            rounds.back()["provider_items"] = std::move(items);
        }
    }
    auto attempts = Json::array();
    for (const auto& attempt : session.model_attempts) {
        attempts.push_back(encode_attempt(attempt));
    }
    auto tasks = Json::array();
    for (std::size_t i = 0; i < session.tasks.size(); ++i) {
        const auto& task = session.tasks[i];
        const bool current = i + 1 == session.tasks.size();
        tasks.push_back({{"id", i + 1}, {"question", task.question}, {"first_round", task.first_round},
            {"end_round", current ? session.rounds.size() : session.tasks[i + 1].first_round},
            {"first_request", task.first_request}, {"first_dispatch", task.first_dispatch},
            {"allow_edit", task.allow_edit},
            {"stop", current ? Json(static_cast<int>(session.stop)) : Json(static_cast<int>(*task.ended))},
            {"closed", !current || task_closed(session)}});
        if (task.active_ms) {
            tasks.back()["active_ms"] = task.active_ms;
        }
        if (task.approval_wait_ms) {
            tasks.back()["approval_wait_ms"] = *task.approval_wait_ms;
        }
        if (task.supplement) {
            const auto& input = *task.supplement;
            tasks.back()["supplement"] = {{"id", supplement_id(i + 1)}, {"task_id", i + 1},
                {"sequence", TaskSupplement::sequence}, {"text", input.text}, {"saved_revision", input.saved_revision},
                {"before_round", input.before_round ? Json(*input.before_round) : Json(nullptr)}};
        }
    }
    return Json{{"question", session.question}, {"rounds", std::move(rounds)}, {"requests", session.requests},
                {"dispatches", session.dispatches}, {"stop", static_cast<int>(session.stop)},
                {"diagnostic", session.diagnostic}, {"finished", session.finished},
                {"model_attempts", std::move(attempts)}, {"tasks", std::move(tasks)}}.dump();
}

ArgumentValue decode_argument(const Json& item) {
    const auto& value = item.at(2);
    switch (item.at(1).get<int>()) {
        case 0: return value.get<std::string>();
        case 1: return value.get<std::int64_t>();
        case 2: return value.get<std::uint64_t>();
        case 3: return value.get<bool>();
        case 4: return value.get<double>();
        case 5: return nullptr;
        case 6: return value.get<TextList>();
        default: throw std::runtime_error("unsupported stored tool argument");
    }
}

Session decode_session(std::string_view payload, std::int64_t version = current_format) {
    const auto json = Json::parse(payload);
    Session session;
    session.question = json.at("question");
    session.requests = json.at("requests");
    session.dispatches = json.at("dispatches");
    const auto stop = json.at("stop").get<int>();
    if (stop < 0 || stop > static_cast<int>(SessionStop::repeated_file_failure)) {
        throw std::runtime_error("invalid stored session stop reason");
    }
    session.stop = static_cast<SessionStop>(stop);
    session.diagnostic = json.at("diagnostic");
    session.finished = json.at("finished");
    if (json.contains("model_attempts")) {
        for (const auto& attempt : json.at("model_attempts")) {
            session.model_attempts.push_back(decode_attempt(attempt));
        }
    }
    for (const auto& saved : json.at("rounds")) {
        Round round{saved.at("text"), {}};
        for (const auto& item : saved.at("tools")) {
            ToolRecord record;
            record.call.id = item.at("id");
            record.call.name = item.at("name");
            record.dispatched = item.at("dispatched");
            record.text_offset = item.at("offset");
            const auto status = item.at("status").get<int>();
            if (status < 0 || status > static_cast<int>(ToolStatus::unknown) ||
                record.text_offset > round.text.size() ||
                (!round.tools.empty() && record.text_offset < round.tools.back().text_offset)) {
                throw std::runtime_error("invalid stored tool record");
            }
            record.result = {static_cast<ToolStatus>(status), item.at("result"), item.at("version"),
                             item.value("observed_utc", std::string{})};
            if (item.contains("file_failure")) {
                const auto failure = item.at("file_failure").get<int>();
                if (failure < static_cast<int>(FileFailure::unsupported_text) ||
                    failure > static_cast<int>(FileFailure::fragment_ambiguous) ||
                    record.result.status != ToolStatus::failed || record.result.version.empty()) {
                    throw std::runtime_error("invalid stored file failure");
                }
                record.result.file_failure = static_cast<FileFailure>(failure);
            }
            for (const auto& arg : item.at("arguments")) {
                record.call.arguments.push_back({arg.at(0), decode_argument(arg)});
            }
            if (!item.at("edit").is_null()) {
                record.edit = decode_edit(item.at("edit"));
            }
            if (item.contains("run")) {
                record.run = decode_run(item.at("run"));
            }
            if (item.contains("verification")) {
                record.verification = decode_verification(item.at("verification"));
            }
            round.tools.push_back(std::move(record));
        }
        if (saved.contains("provider_items")) {
            for (const auto& item : saved.at("provider_items")) {
                ProviderItemPosition position{item.at("json"), item.at("text_offset"), item.at("tools_before")};
                if (position.text_offset > round.text.size() || position.tools_before > round.tools.size() ||
                    (!round.provider_items.empty() &&
                     (position.text_offset < round.provider_items.back().text_offset ||
                      position.tools_before < round.provider_items.back().tools_before))) {
                    throw std::runtime_error("invalid stored provider item");
                }
                round.provider_items.push_back(std::move(position));
            }
        }
        session.rounds.push_back(std::move(round));
    }
    if (version <= 2) {
        session.tasks.push_back({session.question});
        return session;
    }
    const auto& tasks = json.at("tasks");
    if (!tasks.is_array() || tasks.empty()) {
        throw std::runtime_error("session snapshot has no tasks");
    }
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const auto& item = tasks[i];
        const bool current = i + 1 == tasks.size();
        const auto end = current ? session.rounds.size() : tasks[i + 1].at("first_round").get<std::size_t>();
        Task task{item.at("question"), item.at("first_round"), item.at("first_request"),
                  item.at("first_dispatch"), item.at("allow_edit")};
        task.active_ms = item.value("active_ms", std::size_t{0});
        if (item.contains("approval_wait_ms")) {
            task.approval_wait_ms = item.at("approval_wait_ms").get<std::size_t>();
        }
        const auto code = item.at("stop").get<int>();
        const bool closed = item.at("closed");
        if (item.at("id") != i + 1 || task.question.empty() || item.at("end_round") != end ||
            task.first_round > end || end > session.rounds.size() ||
            task.first_request > session.requests || task.first_dispatch > session.dispatches ||
            code < 0 || code > static_cast<int>(SessionStop::repeated_file_failure) ||
            (i == 0 && (task.first_round || task.first_request || task.first_dispatch || task.question != session.question)) ||
            (i != 0 && (task.first_request < session.tasks.back().first_request ||
                        task.first_dispatch < session.tasks.back().first_dispatch)) ||
            (!current && !closed) ||
            (current && (code != stop || closed != task_closed(session)))) {
            throw std::runtime_error("invalid stored task boundaries");
        }
        if (!current) {
            task.ended = static_cast<SessionStop>(code);
        }
        if (item.contains("supplement")) {
            const auto& saved = item.at("supplement");
            TaskSupplement input{saved.at("text"), saved.at("saved_revision")};
            if (!saved.at("before_round").is_null()) input.before_round = saved.at("before_round").get<std::size_t>();
            if (version < current_format || saved.at("id") != supplement_id(i + 1) || saved.at("task_id") != i + 1 ||
                saved.at("sequence") != TaskSupplement::sequence || !valid_supplement(input.text) ||
                input.saved_revision <= 0 ||
                (input.before_round && (*input.before_round < task.first_round || *input.before_round > end))) {
                throw std::runtime_error("invalid stored task supplement");
            }
            task.supplement = std::move(input);
        }
        session.tasks.push_back(std::move(task));
    }
    return session;
}

std::string database_name(std::string_view id) {
    if (id.empty() || id.size() > 48 || !std::ranges::all_of(id, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        })) {
        throw std::invalid_argument("Use a session ID of 1–48 lowercase letters, digits, hyphens or underscores.");
    }
    // The fixed prefix keeps Windows device aliases out of the private state directory.
    return "session-" + std::string(id);
}

using Connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

Connection connect(const fs::path& path, bool create) {
    sqlite3* opened = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW | (create ? SQLITE_OPEN_CREATE : 0);
    const auto status = sqlite3_open_v2(utf8(path).c_str(), &opened, flags, nullptr);
    Connection db(opened, sqlite3_close);
    check(db.get(), status);
    return db;
}

std::int64_t format(sqlite3* db) {
    Statement query(db, "PRAGMA user_version");
    query.step(SQLITE_ROW);
    return query.integer(0);
}

void set_current_format(sqlite3* db) {
    const auto query = "PRAGMA user_version=" + std::to_string(current_format);
    sql(db, query.c_str());
}

void verify_database(sqlite3* db) {
    Statement integrity(db, "PRAGMA quick_check");
    integrity.step(SQLITE_ROW);
    if (integrity.text(0) != "ok") {
        throw std::runtime_error("session database failed SQLite quick_check");
    }
}

void configure(sqlite3* connection) {
    sql(connection, "PRAGMA trusted_schema=OFF");
    Statement journal(connection, "PRAGMA journal_mode=DELETE");
    journal.step(SQLITE_ROW);
    if (journal.text(0) != "delete") {
        throw std::runtime_error("SQLite rollback journal mode is unavailable");
    }
    sql(connection, "PRAGMA synchronous=EXTRA");
}

void reserve_backup(const fs::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("could not create the migration backup");
    }
    CloseHandle(handle);
#else
    const auto fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_CLOEXEC | O_WRONLY, 0600);
    if (fd == -1) {
        throw std::runtime_error("could not create the migration backup");
    }
    ::close(fd);
#endif
}

} // namespace

struct SessionStore::Impl {
    Impl(const fs::path& directory, const std::string& name, const std::string& scope,
         const std::optional<std::string>& question, StoreCheckpoint checkpoint)
        : lock(directory / (name + ".lock")), checkpoint(std::move(checkpoint)) {
        const auto path = directory / (name + ".sqlite3");
        const auto status = fs::symlink_status(path);
        if (fs::exists(status) != !question.has_value()) {
            throw std::runtime_error(question ? "session already exists; use --resume or --follow-up" : "session does not exist");
        }
        if (!question && (!fs::is_regular_file(status) || fs::hard_link_count(path) != 1)) {
            throw std::runtime_error("unsupported session database file");
        }
        db = connect(path, question.has_value());
        auto* connection = db.get();
        const auto version = format(connection);
        if (!question && version >= 1 && version < current_format) {
            throw std::runtime_error("This session requires offline migration with --migrate-session ID.");
        }
        if (version != (question ? 0 : current_format)) {
            throw std::runtime_error("unsupported session database format");
        }
        configure(connection);
        if (question) {
            Transaction transaction(connection);
            sql(connection, "CREATE TABLE session(id INTEGER PRIMARY KEY CHECK(id=1), scope TEXT NOT NULL,"
                            " revision INTEGER NOT NULL, snapshot TEXT NOT NULL) STRICT");
            sql(connection, "CREATE TABLE checkpoints(revision INTEGER PRIMARY KEY, phase TEXT NOT NULL) STRICT");
            set_current_format(connection);
            Session initial;
            initial.question = *question;
            initial.tasks.push_back({initial.question});
            Statement insert(connection, "INSERT INTO session VALUES(1, ?, 0, ?)");
            insert.bind(1, scope);
            insert.bind(2, encode_session(initial));
            insert.step(SQLITE_DONE);
            sql(connection, "INSERT INTO checkpoints VALUES(0, 'input')");
            transaction.commit();
        }
        Statement session(connection, "SELECT scope, revision FROM session WHERE id=1");
        session.step(SQLITE_ROW);
        if (session.text(0) != scope) {
            throw std::runtime_error("session scope changed (workspace identity, model, or file permissions)");
        }
        revision = session.integer(1);
    }

    WriterLock lock;
    StoreCheckpoint checkpoint;
    Connection db{nullptr, sqlite3_close};
    std::int64_t revision = 0;
};

SessionStore::SessionStore(std::string_view id, const SessionScope& scope, std::optional<std::string> question,
                           StoreCheckpoint checkpoint)
    : state_(std::make_unique<Impl>(session_directory(scope.root), database_name(id), scope_record(scope),
                                    question, std::move(checkpoint))) {}

SessionStore::~SessionStore() = default;

Session SessionStore::load() const {
    Statement session(state_->db.get(), "SELECT snapshot FROM session WHERE id=1");
    session.step(SQLITE_ROW);
    auto snapshot = decode_session(session.text(0));
    std::int64_t previous = 0;
    for (const auto& task : snapshot.tasks) {
        if (!task.supplement) continue;
        const auto saved_revision = task.supplement->saved_revision;
        if (saved_revision <= previous || saved_revision > revision()) {
            throw std::runtime_error("invalid saved supplement revision");
        }
        previous = saved_revision;
    }
    return snapshot;
}

void SessionStore::save_supplement(Session& session, std::size_t task_id, std::string text) {
    if (task_id == 0 || task_id != session.tasks.size()) {
        throw std::invalid_argument("A supplement must identify the current task.");
    }
    if (!valid_supplement(text)) {
        throw std::invalid_argument("A supplement requires nonblank UTF-8 text of at most 4096 bytes.");
    }
    auto& input = session.tasks.back().supplement;
    if (input) {
        if (input->text == text) return;
        throw std::invalid_argument("The current task already has a saved supplement.");
    }
    const auto active = std::span(session.rounds).subspan(session.tasks.back().first_round);
    if (task_closed(session) || session.stop == SessionStop::storage_error ||
        std::ranges::any_of(active, [](const Round& round) {
            return std::ranges::any_of(round.tools, [](const ToolRecord& tool) {
                return tool.dispatched && tool.result.status == ToolStatus::pending;
            });
        })) {
        throw std::invalid_argument("This task cannot accept a supplement. Review its saved state.");
    }
    const auto before = revision();
    input.emplace(TaskSupplement{std::move(text), before + 1});
    try {
        save(session, "supplement");
    } catch (...) {
        if (revision() == before) input.reset();
        throw;
    }
}

std::string SessionStore::migrate(std::string_view id) {
    const auto directory = session_directory({});
    const auto name = database_name(id);
    const WriterLock lock(directory / (name + ".lock"));
    const auto path = directory / (name + ".sqlite3");
    if (!fs::is_regular_file(fs::symlink_status(path)) || fs::hard_link_count(path) != 1) {
        throw std::runtime_error("could not locate a regular session database");
    }
    const auto db = connect(path, false);
    const auto version = format(db.get());
    if (version == current_format) {
        return {};
    }
    if (version < 1 || version >= current_format) {
        throw std::runtime_error("unsupported session database format");
    }
    sql(db.get(), "PRAGMA trusted_schema=OFF");
    verify_database(db.get());
    std::string original;
    std::string scope;
    std::int64_t revision = 0;
    {
        Statement input(db.get(), "SELECT snapshot, scope, revision FROM session WHERE id=1");
        input.step(SQLITE_ROW);
        original = input.text(0);
        scope = input.text(1);
        revision = input.integer(2);
    }
    const auto snapshot = decode_session(original, version);
    const auto source_version = std::to_string(version);
    const auto backup_path = directory / (name + ".v" + source_version + "-" + platform::random_suffix() + ".sqlite3");
    reserve_backup(backup_path);
    {
        const auto backup = connect(backup_path, false);
        configure(backup.get());
        auto* copying = sqlite3_backup_init(backup.get(), "main", db.get(), "main");
        if (!copying) {
            check(backup.get(), sqlite3_errcode(backup.get()));
            throw std::runtime_error("could not initialize the migration backup");
        }
        const auto copied = sqlite3_backup_step(copying, -1);
        const auto finished = sqlite3_backup_finish(copying);
        check(backup.get(), copied, SQLITE_DONE);
        check(backup.get(), finished);
        verify_database(backup.get());
        Statement saved(backup.get(), "SELECT snapshot, scope, revision FROM session WHERE id=1");
        saved.step(SQLITE_ROW);
        if (format(backup.get()) != version || saved.text(0) != original || saved.text(1) != scope ||
            saved.integer(2) != revision) {
            throw std::runtime_error("migration backup differs from the original session");
        }
    }

    configure(db.get());
    Transaction transaction(db.get());
    const auto upgraded = encode_session(snapshot);
    Statement update(db.get(), "UPDATE session SET snapshot=?, revision=revision+1 WHERE id=1 AND revision=? AND snapshot=?");
    update.bind(1, upgraded);
    update.bind(2, revision);
    update.bind(3, original);
    update.step(SQLITE_DONE);
    if (sqlite3_changes(db.get()) != 1 || decode_session(upgraded) != snapshot) {
        throw std::runtime_error("session changed or could not be verified during migration");
    }
    Statement checkpoint(db.get(), "INSERT INTO checkpoints VALUES(?, ?)");
    checkpoint.bind(1, revision + 1);
    checkpoint.bind(2, "migrated_v" + source_version + "_to_v" + std::to_string(current_format));
    checkpoint.step(SQLITE_DONE);
    set_current_format(db.get());
    verify_database(db.get());
    transaction.commit();
    return utf8(backup_path);
}

std::int64_t SessionStore::revision() const {
    return state_->revision;
}

void SessionStore::save(const Session& session, std::string_view point) {
    auto* db = state_->db.get();
    const bool force_full = state_->checkpoint && state_->checkpoint("before", point, session);
    if (force_full) {
        sql(db, "PRAGMA max_page_count=1");
    }
#ifdef LAY_BENCHMARK_METRICS
    const BenchmarkTimer timer;
#endif
    Transaction transaction(db);
    if (force_full) {
        sql(db, "UPDATE session SET snapshot=CAST(zeroblob(4194304) AS TEXT) WHERE id=1");
        throw std::runtime_error("SQLite page quota was not enforced");
    }
    Statement update(db, "UPDATE session SET snapshot=?, revision=revision+1 WHERE id=1 AND revision=?");
    update.bind(1, encode_session(session));
    update.bind(2, state_->revision);
    update.step(SQLITE_DONE);
    if (sqlite3_changes(db) != 1) {
        throw std::runtime_error("session revision changed before commit");
    }
    Statement commit(db, "INSERT INTO checkpoints VALUES(?, ?)");
    commit.bind(1, state_->revision + 1);
    commit.bind(2, point);
    commit.step(SQLITE_DONE);
    transaction.commit();
    ++state_->revision;
#ifdef LAY_BENCHMARK_METRICS
    timer.record("checkpoint", session.tasks.size());
#endif
    if (state_->checkpoint) {
        state_->checkpoint("after", point, session);
    }
}

} // namespace lay
