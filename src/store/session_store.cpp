#include "store/session_store.hpp"
#include "platform/workspace.hpp"

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
    const auto workspace = fs::canonical(root);
    const auto outside = [&](const fs::path& candidate) {
        return std::ranges::mismatch(candidate, workspace, [](const auto& a, const auto& b) {
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
    return Json{{"root", utf8(fs::canonical(scope.root))}, {"identity", scope.root_identity},
                {"provider", scope.provider}, {"model", scope.model}, {"write", scope.writable_path},
                {"excluded", scope.excluded}}.dump();
}

Json encode_edit(const EditEvidence& edit) {
    return {{"path", edit.path}, {"parent", edit.parent}, {"before", edit.before},
            {"before_digest", edit.before_digest}, {"after_digest", edit.after_digest},
            {"temporary", edit.temporary}, {"backup", edit.backup}};
}

EditEvidence decode_edit(const Json& value) {
    return {value.at("path"), value.at("parent"), value.at("before"), value.at("before_digest"),
            value.at("after_digest"), value.at("temporary"), value.at("backup")};
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
        }
        rounds.push_back({{"text", round.text}, {"tools", std::move(tools)}});
    }
    return Json{{"question", session.question}, {"rounds", std::move(rounds)}, {"requests", session.requests},
                {"dispatches", session.dispatches}, {"stop", static_cast<int>(session.stop)},
                {"diagnostic", session.diagnostic}, {"finished", session.finished}}.dump();
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
        default: throw std::runtime_error("unsupported stored tool argument");
    }
}

Session decode_session(std::string_view payload) {
    const auto json = Json::parse(payload);
    Session session;
    session.question = json.at("question");
    session.requests = json.at("requests");
    session.dispatches = json.at("dispatches");
    const auto stop = json.at("stop").get<int>();
    if (stop < 0 || stop > static_cast<int>(SessionStop::approval_required)) {
        throw std::runtime_error("invalid stored session stop reason");
    }
    session.stop = static_cast<SessionStop>(stop);
    session.diagnostic = json.at("diagnostic");
    session.finished = json.at("finished");
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
            record.result = {static_cast<ToolStatus>(status), item.at("result"), item.at("version")};
            for (const auto& arg : item.at("arguments")) {
                record.call.arguments.push_back({arg.at(0), decode_argument(arg)});
            }
            if (!item.at("edit").is_null()) {
                record.edit = decode_edit(item.at("edit"));
            }
            round.tools.push_back(std::move(record));
        }
        session.rounds.push_back(std::move(round));
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

} // namespace

struct SessionStore::Impl {
    Impl(const fs::path& directory, const std::string& name, const std::string& scope,
         const std::optional<std::string>& question, StoreCheckpoint checkpoint)
        : lock(directory / (name + ".lock")), checkpoint(std::move(checkpoint)) {
        const auto path = directory / (name + ".sqlite3");
        const auto status = fs::symlink_status(path);
        if (fs::exists(status) != !question.has_value()) {
            throw std::runtime_error(question ? "session already exists; use --resume" : "session does not exist");
        }
        if (!question && (!fs::is_regular_file(status) || fs::hard_link_count(path) != 1)) {
            throw std::runtime_error("unsupported session database file");
        }
        sqlite3* opened = nullptr;
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW | (question ? SQLITE_OPEN_CREATE : 0);
        const auto result = sqlite3_open_v2(utf8(path).c_str(), &opened, flags, nullptr);
        db.reset(opened);
        check(db.get(), result);
        auto* connection = db.get();
        std::int64_t version = 0;
        {
            Statement format(connection, "PRAGMA user_version");
            format.step(SQLITE_ROW);
            version = format.integer(0);
        }
        if (version != (question ? 0 : 1)) {
            throw std::runtime_error("unsupported session database format");
        }
        sql(connection, "PRAGMA trusted_schema=OFF");
        {
            Statement journal(connection, "PRAGMA journal_mode=DELETE");
            journal.step(SQLITE_ROW);
            if (journal.text(0) != "delete") {
                throw std::runtime_error("SQLite rollback journal mode is unavailable");
            }
        }
        sql(connection, "PRAGMA synchronous=EXTRA");
        if (question) {
            Transaction transaction(connection);
            sql(connection, "CREATE TABLE session(id INTEGER PRIMARY KEY CHECK(id=1), scope TEXT NOT NULL,"
                            " revision INTEGER NOT NULL, snapshot TEXT NOT NULL) STRICT");
            sql(connection, "CREATE TABLE checkpoints(revision INTEGER PRIMARY KEY, phase TEXT NOT NULL) STRICT");
            sql(connection, "PRAGMA user_version=1");
            Session initial;
            initial.question = *question;
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
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db{nullptr, sqlite3_close};
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
    return decode_session(session.text(0));
}

void SessionStore::save(const Session& session, std::string_view point) {
    auto* db = state_->db.get();
    const bool force_full = state_->checkpoint && state_->checkpoint("before", point, session);
    if (force_full) {
        sql(db, "PRAGMA max_page_count=1");
    }
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
    if (state_->checkpoint) {
        state_->checkpoint("after", point, session);
    }
}

} // namespace lay
