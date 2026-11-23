#include "tools/edit.hpp"
#include "core/utf8.hpp"
#include "tools/file_version.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace lay {
namespace {

constexpr std::string_view absent = "absent";
constexpr std::string_view denied = "edit denied by user";
constexpr std::string_view bom = "\xef\xbb\xbf";

bool plain_text(std::string_view bytes) {
    return valid_utf8(bytes) && std::ranges::none_of(bytes, [](unsigned char byte) {
        return (byte < 32 && byte != '\n' && byte != '\r' && byte != '\t') || byte == 127;
    });
}

struct Normalized {
    std::string text;
    bool crlf = false;
    bool utf8_bom = false;
};

std::optional<Normalized> normalize(std::string_view bytes) {
    if (!plain_text(bytes)) {
        return std::nullopt;
    }
    Normalized result;
    result.utf8_bom = bytes.starts_with(bom);
    if (result.utf8_bom) {
        bytes.remove_prefix(bom.size());
    }
    result.text.reserve(bytes.size());
    bool lf = false;
    for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
        const auto byte = bytes[offset];
        if (byte == '\r') {
            if (lf || offset + 1 == bytes.size() || bytes[offset + 1] != '\n') {
                return std::nullopt;
            }
            result.crlf = true;
            ++offset;
            result.text += '\n';
        } else {
            if (byte == '\n') {
                if (result.crlf) {
                    return std::nullopt;
                }
                lf = true;
            }
            result.text += byte;
        }
    }
    return result;
}

std::string encode(const Normalized& before, std::string_view text) {
    std::string bytes = before.utf8_bom ? std::string(bom) : std::string{};
    bytes.reserve(bytes.size() + text.size());
    for (const auto byte : text) {
        if (byte == '\n' && before.crlf) {
            bytes += '\r';
        }
        bytes += byte;
    }
    return bytes;
}

void show_lines(std::string& preview, std::string_view lines, char marker) {
    while (!lines.empty()) {
        const auto newline = lines.find('\n');
        preview += marker;
        preview += lines.substr(0, newline);
        preview += '\n';
        if (newline == lines.npos) {
            preview += "\\ No newline at end of file\n";
            return;
        }
        lines.remove_prefix(newline + 1);
    }
}

std::string diff(std::string_view path, std::string_view before, std::size_t position,
                 std::string_view old_text, std::string_view new_text, bool create) {
    auto head = position == 0 ? before.npos : before.rfind('\n', position - 1);
    head = head == before.npos ? 0 : head + 1;
    const auto end = position + old_text.size();
    const auto newline = before.find('\n', end);
    const auto tail = newline == before.npos ? before.size() : newline + 1;
    const auto affected = before.substr(head, tail - head);
    auto replacement = std::string(before.substr(head, position - head)) + std::string(new_text) +
        std::string(before.substr(end, tail - end));
    if (affected.size() + replacement.size() > 16 * 1024) {
        return {};
    }
    std::string preview = "--- " + (create ? std::string("/dev/null") : std::string(path)) +
        "\n+++ " + std::string(path) + "\n@@ line " +
        std::to_string(1 + std::ranges::count(before.substr(0, head), '\n')) + " @@\n";
    show_lines(preview, affected, '-');
    show_lines(preview, replacement, '+');
    return preview;
}

bool has_read(const Session& session, std::string_view path, std::string_view version) {
    for (const auto& round : session.rounds) {
        for (const auto& record : round.tools) {
            if (record.call.name == "read" && record.result.status == ToolStatus::ok &&
                record.result.version == version && platform::path_equal(text_arg(record.call, "path"), path)) {
                return true;
            }
        }
    }
    return false;
}

bool denied_before(const Session& session, const EditEvidence& evidence) {
    for (const auto& round : session.rounds) {
        for (const auto& record : round.tools) {
            if (record.edit && record.result.status == ToolStatus::failed && record.result.text == denied &&
                record.edit->path == evidence.path && record.edit->parent == evidence.parent &&
                record.edit->before == evidence.before && record.edit->after_digest == evidence.after_digest) {
                return true;
            }
        }
    }
    return false;
}

EditPreparation reject(std::string message, std::string_view version = {},
                       std::optional<FileFailure> failure = {}) {
    return {{}, std::move(message), {}, false, std::string(version), failure};
}

} // namespace

std::vector<std::string> edit_progress(const Session& session, std::span<const std::string> paths) {
    std::vector<std::string> progress;
    const auto first = session.tasks.empty() ? 0 : session.tasks.back().first_round;
    for (const auto& path : paths) {
        const ToolRecord* latest = nullptr;
        const ToolRecord* last_effect = nullptr;
        for (const auto& round : std::span(session.rounds).subspan(first)) {
            for (const auto& tool : round.tools) {
                if (tool.call.name != "edit" || !platform::path_equal(path, text_arg(tool.call, "path"))) continue;
                latest = &tool;
                if (tool.dispatched && (tool.result.status == ToolStatus::ok ||
                    tool.result.status == ToolStatus::unknown || tool.result.status == ToolStatus::pending)) {
                    last_effect = &tool;
                }
            }
        }
        std::string note = "[edit progress] " + path;
        if (last_effect && last_effect->result.status == ToolStatus::unknown) {
            note += " [effect unknown]";
            note += "\n" + last_effect->result.text;
        } else if (last_effect && last_effect->result.status == ToolStatus::pending) {
            note += " [in progress]";
        } else if (last_effect) {
            note += " [applied]";
        } else if (latest && latest->dispatched) {
            note += " [failed]";
        } else {
            note += " [not started]";
        }
        if (latest && latest != last_effect && !latest->result.text.empty()) {
            note += "\n" + latest->result.text;
        }
        progress.push_back(std::move(note));
    }
    return progress;
}

EditWorkspace::EditWorkspace(const ReadOnlyWorkspace& workspace, std::string writable_path)
    : workspace_(workspace), path_(std::move(writable_path)) {
    if (path_ == "." || !workspace_.allowed(path_)) {
        throw std::invalid_argument("Each --write must name a writable workspace-relative file.");
    }
    const auto separator = path_.find_last_of('/');
    dirname_ = separator == path_.npos ? "." : path_.substr(0, separator);
    filename_ = path_.substr(separator == path_.npos ? 0 : separator + 1);
}

bool EditWorkspace::allows(std::string_view path) const {
    return platform::path_equal(path, path_);
}

platform::Node EditWorkspace::parent() const {
    auto node = dirname_ == "." ? workspace_.root_.duplicate() : workspace_.open(dirname_);
    if (!node || !node.directory()) {
        throw std::runtime_error("edit parent directory is missing or unsupported");
    }
    return node;
}

bool EditWorkspace::current(const platform::Node& directory, const EditEvidence& evidence) const {
    if (directory.identity() != evidence.parent) {
        return false;
    }
    if (evidence.before == absent) {
        return !directory.child_exists(filename_);
    }
    const auto node = directory.child(filename_);
    if (!node.editable()) {
        return false;
    }
    const auto raw = node.read(editable_bytes + 1);
    return raw.size() <= editable_bytes && file_version(node, raw) == evidence.before;
}

EditPreparation EditWorkspace::prepare(const ToolCall& call, const Session& session,
                                       const ReviewEdit& review, const StopRequested& stop) const {
    const auto path = text_arg(call, "path");
    const auto version = text_arg(call, "version");
    const auto old_text = text_arg(call, "old_text");
    const auto new_text = text_arg(call, "new_text");
    if (!allows(path)) {
        return reject("edit path is outside the allowed file");
    }
    if (!plain_text(old_text) || !plain_text(new_text) || old_text.find('\r') != old_text.npos ||
        new_text.find('\r') != new_text.npos || new_text.starts_with(bom)) {
        return reject("edit fragments must be plain UTF-8 with LF newlines");
    }

    const bool create = version == absent;
    auto plan = [&]() -> EditPreparation {
        const auto directory = parent();
        std::string raw;
        if (create) {
            if (!old_text.empty()) {
                return reject("omit old_text when creating a file");
            }
            if (directory.child_exists(filename_)) {
                return reject("target already exists");
            }
        } else {
            if (old_text.empty() || !has_read(session, path_, version)) {
                return reject("a current read version and a non-empty old_text are required");
            }
            const auto target = directory.child(filename_);
            if (!target.editable()) {
                return reject("target is missing or its file type or metadata is unsupported");
            }
            raw = target.read(editable_bytes + 1);
            if (raw.size() > editable_bytes) {
                return reject("file exceeds the 2097152-byte edit limit");
            }
            if (file_version(target, raw) != version) {
                return reject("file changed since it was read");
            }
        }
        auto normalized = normalize(raw);
        if (!normalized) {
            return reject("file encoding or mixed newlines do not support lossless editing");
        }
        const auto position = create ? 0 : normalized->text.find(old_text);
        if (position == std::string::npos) {
            return reject("old_text was not found", version, FileFailure::fragment_missing);
        }
        if (!create && normalized->text.find(old_text, position + 1) != std::string::npos) {
            return reject("old_text is ambiguous", version, FileFailure::fragment_ambiguous);
        }
        auto preview = diff(path_, normalized->text, position, old_text, new_text, create);
        if (preview.empty()) {
            return reject("the edit preview exceeds 16384 bytes of affected lines");
        }
        normalized->text.replace(position, old_text.size(), new_text);
        auto bytes = encode(*normalized, normalized->text);
        if (!create && bytes == raw) {
            return reject("the proposed edit leaves the file unchanged");
        }
        if (bytes.size() > editable_bytes) {
            return reject("the resulting file exceeds the 2097152-byte edit limit");
        }
        const auto stem = ".lay-code-" + platform::random_suffix();
        EditEvidence evidence{path_, directory.identity(), std::string(version),
                              create ? "" : platform::sha256(raw), platform::sha256(bytes),
                              stem + ".tmp", {}, std::move(preview)};
#ifdef _WIN32
        if (!create) {
            evidence.backup = stem + ".bak";
        }
#endif
        if (denied_before(session, evidence)) {
            return {std::move(evidence), std::string(denied), {}};
        }
        const auto approval = review(evidence, evidence.review_diff);
        if (approval != Approval::unavailable) evidence.approved = approval == Approval::approved;
        if (approval == Approval::unavailable || stop()) {
            return {std::move(evidence), "interactive approval required", {}, true};
        }
        if (approval != Approval::approved) {
            return {std::move(evidence), std::string(denied), {}};
        }
        try {
            if (!current(parent(), evidence)) {
                return {std::move(evidence), "file changed during approval", {}};
            }
        } catch (const std::runtime_error& error) {
            return {std::move(evidence), error.what(), {}};
        }
        auto execute = [this, plan = evidence, bytes = std::move(bytes), stop] {
            return apply(plan, bytes, stop);
        };
        return {std::move(evidence), {}, std::move(execute)};
    };
    try {
        return plan();
    } catch (const std::runtime_error& error) {
        return reject(error.what());
    }
}

ToolResult EditWorkspace::apply(const EditEvidence& evidence, std::string_view bytes,
                               const StopRequested& stop) const {
    using Status = platform::Replacement::Status;
    try {
        const auto directory = parent();
        if (stop() || !current(directory, evidence)) {
            return {ToolStatus::failed, "edit stopped or file changed before execution"};
        }
        const auto outcome = directory.replace(filename_, evidence.temporary, evidence.backup,
            bytes, evidence.before == absent, [&] { return !stop() && current(directory, evidence); });
        if (outcome.status == Status::failed) {
            return {ToolStatus::failed, outcome.diagnostic};
        }
        if (outcome.status == Status::unknown) {
            auto observed = recover(evidence);
            observed.text = outcome.diagnostic + ". " + observed.text;
            return observed;
        }
        const auto node = directory.child(filename_);
        if (node && !node.directory()) {
            const auto raw = node.read(editable_bytes + 1);
            if (raw.size() <= editable_bytes && platform::sha256(raw) == evidence.after_digest) {
                return {ToolStatus::ok, "applied " + path_ + "\nsha256=" + evidence.after_digest,
                        file_version(node, raw)};
            }
        }
    } catch (const std::runtime_error&) {
        // A platform failure after dispatch requires observation before the next model request.
    }
    return recover(evidence);
}

ToolResult EditWorkspace::recover(const EditEvidence& evidence) const {
    std::string report = "effect unknown for " + evidence.path + ". ";
    try {
        const auto directory = parent();
        if (evidence.path != path_ || directory.identity() != evidence.parent) {
            return {ToolStatus::unknown, report + "parent directory changed"};
        }
        const auto node = directory.child(filename_);
        if (!node || node.directory()) {
            report += directory.child_exists(filename_) ? "target cannot be inspected" : "target absent";
        } else {
            const auto raw = node.read(editable_bytes + 1);
            if (raw.size() <= editable_bytes && platform::sha256(raw) == evidence.after_digest) {
                report += "observed bytes match expected after state";
            } else if (raw.size() <= editable_bytes && file_version(node, raw) == evidence.before) {
                report += "observed file matches expected before state";
            } else {
                report += "observed file differs from the expected states";
            }
        }
        for (const auto& name : {evidence.temporary, evidence.backup}) {
            if (!name.empty() && directory.child_exists(name)) {
                report += ". Retained " + (dirname_ == "." ? name : dirname_ + "/" + name);
            }
        }
    } catch (const std::runtime_error&) {
        report += "workspace observation failed";
    }
    return {ToolStatus::unknown, std::move(report)};
}

void EditWorkspace::cleanup(const EditEvidence& evidence) const {
    if (evidence.backup.empty() || evidence.path != path_) {
        return;
    }
    const auto directory = parent();
    if (directory.identity() == evidence.parent) {
        bool ours = false;
        {
            const auto backup = directory.child(evidence.backup);
            if (backup && !backup.directory()) {
                const auto raw = backup.read(editable_bytes + 1);
                ours = raw.size() <= editable_bytes && platform::sha256(raw) == evidence.before_digest;
            }
        }
        if (ours) {
            directory.remove_backup(evidence.backup);
        }
    }
}

} // namespace lay
