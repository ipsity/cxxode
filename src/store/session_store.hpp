#pragma once

#include "core/session.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace lay {

struct SessionScope {
    std::filesystem::path root;
    std::string root_identity;
    std::string provider;
    std::string model;
    std::vector<std::string> writable_paths;
    std::vector<std::string> excluded;
    bool allow_run = false;
};

// The contract driver can request a SQLite space failure before one commit.
using StoreCheckpoint = std::function<bool(std::string_view stage, std::string_view point, const Session&)>;

class SessionStore {
public:
    SessionStore(std::string_view id, const SessionScope& scope, std::optional<std::string> question,
                 StoreCheckpoint checkpoint = {});
    ~SessionStore();

    Session load() const;
    std::int64_t revision() const;
    void save(const Session& session, std::string_view point);
    static std::string migrate(std::string_view id);

private:
    struct Impl;
    std::unique_ptr<Impl> state_;
};

} // namespace lay
