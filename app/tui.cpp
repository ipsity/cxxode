#include "tui.hpp"
#include "tui_terminal.hpp"
#include "tui_text.hpp"
#include "tui_workspace.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace lay::tui {
namespace {

enum class Key {
    text, enter, escape, tab, stop, details, submit, up, down, left, right, first, last,
    previous_page, next_page, backspace, erase, paste, acknowledge, edit_scope
};

struct Event {
    Key key;
    std::string text{};
};

class Input {
public:
    std::vector<Event> decode(std::string_view bytes) {
        pending_ += bytes;
        std::vector<Event> events;
        std::string_view input = pending_;
        while (!input.empty()) {
            const auto first = static_cast<unsigned char>(input.front());
            auto length = std::size_t{1};
            if (first == 0x1b) {
                length = escape(input, events, bytes.empty());
            } else if (first < 0x20 || first == 0x7f) {
                if (pasting_) {
                    if (first == '\n' || first == '\r' || first == '\t') events.push_back({Key::text, " "});
                } else {
                    switch (first) {
                        case '\r': case '\n': events.push_back({Key::enter}); break;
                        case '\t': events.push_back({Key::tab}); break;
                        case '\x03': events.push_back({Key::stop}); break;
                        case '\x0f': events.push_back({Key::details}); break;
                        case '\b': case 0x7f: events.push_back({Key::backspace}); break;
                        default: break;
                    }
                }
            } else {
                length = scalar_bytes(first);
                if (length > input.size()) break;
                const auto text = input.substr(0, length);
                if (!valid_utf8(text)) {
                    length = 1;
                } else {
                    const auto value = scalar(text);
                    if (!(value >= 0x80 && value <= 0x9f) && !(value >= 0x202a && value <= 0x202e) &&
                        !(value >= 0x2066 && value <= 0x2069)) {
                        events.push_back({Key::text, std::string(text)});
                    }
                }
            }
            if (length == 0) break;
            input.remove_prefix(length);
        }
        pending_.erase(0, pending_.size() - input.size());
        if (pending_.size() > 4096) pending_.clear();
        return events;
    }

private:
    std::size_t escape(std::string_view input, std::vector<Event>& events, bool idle) {
        if (input.size() == 1) {
            if (!idle) return 0;
            if (!pasting_) events.push_back({Key::escape});
            return 1;
        }
        if (input[1] == ']') {
            const auto bell = input.find('\a', 2);
            const auto end = input.find("\x1b\\", 2);
            return bell < end ? bell + 1 : end == input.npos ? 0 : end + 2;
        }
        if (input[1] != '[' && input[1] != 'O') return 2;
        const auto end = std::ranges::find_if(input.substr(2), [](unsigned char value) {
            return value >= 0x40 && value <= 0x7e;
        });
        if (end == input.end()) return 0;
        const auto count = static_cast<std::size_t>(end - input.begin()) + 1;
        const auto sequence = input.substr(1, count - 1);
        if (sequence == "[200~" || (pasting_ && sequence == "[201~")) {
            pasting_ = sequence == "[200~";
            events.push_back({Key::paste});
        } else if (!pasting_) {
            static constexpr std::pair<std::string_view, Key> keys[]{
                {"[A", Key::up}, {"OA", Key::up}, {"[B", Key::down}, {"OB", Key::down},
                {"[C", Key::right}, {"OC", Key::right}, {"[D", Key::left}, {"OD", Key::left},
                {"[H", Key::first}, {"OH", Key::first}, {"[1~", Key::first},
                {"[F", Key::last}, {"OF", Key::last}, {"[4~", Key::last},
                {"[5~", Key::previous_page}, {"[6~", Key::next_page}, {"[3~", Key::erase},
                {"OQ", Key::submit}, {"[12~", Key::submit},
                {"OR", Key::acknowledge}, {"[13~", Key::acknowledge},
                {"OS", Key::edit_scope}, {"[14~", Key::edit_scope},
            };
            if (const auto match = std::ranges::find(keys, sequence, [](const auto& item) { return item.first; });
                match != std::end(keys)) {
                events.push_back({match->second});
            }
        }
        return count;
    }

    std::string pending_;
    bool pasting_ = false;
};

enum class Scene { normal, edit, command, failure, unknown, stopped, recorded, denied };

struct Row {
    std::string text;
    std::string_view style;
    bool operator==(const Row&) const = default;
};

struct Frame {
    std::vector<Row> rows;
    int caret = 0;
};

class Demo {
public:
    Demo(Scene scene, bool mono, bool ascii) : scene_(scene), mono_(mono), ascii_(ascii), details_(approval()) {}

    Frame frame(Size viewport) {
        Frame result;
        auto& output = result.rows;
        const auto width = std::clamp(viewport.columns - 1, 1, 119);
        const auto line = [&](std::string_view text, std::string_view style = {}) {
            output.push_back({wrap(text, width).front(), mono_ ? std::string_view{} : style});
        };
        line(label("▚ lay code  ·  离线演示", "[] lay code  /  Offline demo"));
        if (viewport.columns < 34 || viewport.rows < 12) {
            if (viewport.rows >= 2) line(label("请放大窗口 · Esc 退出", "Enlarge window / Esc quits"));
            if (viewport.rows >= 3) line(label("Ctrl+C 停止", "Ctrl+C stops"));
            page_ = 1;
            return result;
        }
        line(status(), "\x1b[36m");
        std::vector<std::string> body;
        const auto add = [&](std::string_view text) {
            auto rows = wrap(text, width);
            for (auto& row : rows) body.push_back(std::move(row));
        };
        add("");
        add(std::string(label("你  ", "You  ")) + (goal_.empty() ?
            std::string(label("修复空配置读取，运行解析器测试。", "Fix empty config parsing and run the parser test.")) : goal_));
        add("");
        activity(add);
        page_ = std::min(viewport.rows, 48) - 8;
        const auto total = static_cast<int>(body.size());
        offset_ = std::clamp(offset_, 0, std::max(0, total - page_));
        for (int row = offset_; row < offset_ + page_; ++row) {
            const auto text = row < total ? std::string_view(body[static_cast<std::size_t>(row)]) : std::string_view{};
            const auto style = text.starts_with("- ") ? "\x1b[31m" : text.starts_with("+ ") ? "\x1b[32m" : "";
            line(text, style);
        }
        line(std::to_string(offset_ + 1) + "-" + std::to_string(std::min(total, offset_ + page_)) +
            "/" + std::to_string(total) + std::string(label(" 行  ·  ↑↓ / PgUp PgDn", " lines  /  Up Down PgUp PgDn")));
        std::string rule;
        for (int i = 0; i < width; ++i) rule += label("─", "-");
        line(rule);
        line(label("Ctrl+O 详情 · Ctrl+C 停止 · Esc 退出", "Ctrl+O details / Ctrl+C stop / Esc quit"));
        line(approval() ? (focused_ ? label("输入 y 或 yes 后回车批准。空回车拒绝。",
                                            "y or yes + Enter approves. Enter denies.") :
                                     label("Tab 输入决定，审批仅用于演示。", "Tab to decide. Simulated approval.")) :
                          label("F2 提交草稿，回车确认输入法候选。", "F2 submits draft. Enter keeps editing."));
        if (!notice_.empty()) {
            line(notice_);
        } else {
            line(approval() ? label("范围  parser-demo · 当前动作", "Scope  parser-demo / current action") :
                              label("下一条任务草稿", "Draft for the next task"));
        }
        if (!approval() || focused_) {
            const auto& draft = approval() ? decision_ : draft_;
            auto [visible, column] = draft.view(width - 2, ascii_);
            line("> " + visible, focused_ ? "\x1b[7m" : "");
            result.caret = 3 + column;
        } else {
            line(label("▸ 等待你的决定", "> Waiting for your decision"));
        }
        return result;
    }

    bool handle(const std::vector<Event>& batch, Size size) {
        const bool compact = size.columns < 34 || size.rows < 12;
        const bool combined = batch.size() > 1;
        for (const auto& event : batch) {
            const auto key = event.key;
            if (key == Key::stop) {
                if (approval()) {
                    scene_ = Scene::stopped;
                    reset_review();
                } else {
                    notice_ = label("当前没有活动任务。", "No active task.");
                }
                return true;
            }
            if (key == Key::escape) {
                if (!focused_) return false;
                reset_decision();
                return true;
            }
            if (compact) continue;
            if (key == Key::tab && approval()) {
                if (focused_) {
                    reset_decision();
                } else {
                    focused_ = true;
                    details_ = true;
                    notice_ = {};
                }
                return true;
            }
            if (focused_ && (key == Key::paste || (combined && key == Key::enter))) {
                reset_decision();
                notice_ = label("已忽略粘贴或排队回车，请重新决定。", "Queued input ignored. Use Tab to decide again.");
                return true;
            }
            if (focused_ && key == Key::enter) {
                const auto approved = decision_.text == "y" || decision_.text == "yes";
                scene_ = approved ? (scene_ == Scene::edit ? Scene::command : Scene::normal) : Scene::denied;
                reset_review();
                return true;
            }
            if (key == Key::details) {
                details_ = !details_;
                offset_ = 0;
            } else if (key == Key::up || key == Key::down || key == Key::previous_page || key == Key::next_page ||
                       key == Key::first || key == Key::last) {
                if (key == Key::first) offset_ = 0;
                if (key == Key::last) offset_ = 4096;
                if (key == Key::up) --offset_;
                if (key == Key::down) ++offset_;
                if (key == Key::previous_page) offset_ -= page_;
                if (key == Key::next_page) offset_ += page_;
            } else if (key == Key::submit && !approval() && !combined &&
                       draft_.text.find_first_not_of(' ') != draft_.text.npos) {
                goal_ = draft_.view(8192, ascii_).first;
                draft_ = {};
                scene_ = Scene::recorded;
                reset_review();
                return true;
            } else if (!approval() || focused_) {
                auto& draft = approval() ? decision_ : draft_;
                if (key == Key::text && (!focused_ || draft.text.size() < 8)) draft.insert(event.text);
                if (key == Key::left || key == Key::right) draft.move(key == Key::right, false);
                if (key == Key::backspace || key == Key::erase) draft.move(key == Key::erase, true);
            }
        }
        return true;
    }

private:
    bool approval() const { return scene_ == Scene::edit || scene_ == Scene::command; }
    std::string_view label(std::string_view zh, std::string_view en) const { return ascii_ ? en : zh; }

    std::string_view status() const {
        switch (scene_) {
            case Scene::normal: return label("已完成 · parser-demo", "Complete / parser-demo");
            case Scene::edit: return label("待批准 · 编辑 1/2", "Needs approval / Edit 1/2");
            case Scene::command: return label("待批准 · 运行 2/2", "Needs approval / Run 2/2");
            case Scene::failure: return label("验证失败 · 退出码 8", "Verification failed / Exit 8");
            case Scene::unknown: return label("效果未知 · 等待核对", "Effect unknown / Check result");
            case Scene::stopped: return label("已停止 · 审批已关闭", "Stopped / Approval closed");
            case Scene::recorded: return label("已记录草稿", "Draft recorded");
            case Scene::denied: return label("已拒绝 · 审批已关闭", "Denied / Approval closed");
        }
        return {};
    }

    void activity(const auto& add) const {
        switch (scene_) {
            case Scene::normal:
                add(label("读取  src/parser.cpp · 已完成", "Read  src/parser.cpp / complete"));
                add(label("编辑  src/parser.cpp · 已完成", "Edit  src/parser.cpp / complete"));
                add(label("测试  parser_empty_config · 1 项通过", "Test  parser_empty_config / 1 passed"));
                if (details_) add(label("证据  src/parser.cpp:18 · 编辑版本 1", "Source  src/parser.cpp:18 / edit revision 1"));
                break;
            case Scene::edit:
                add(label("读取  src/parser.cpp · 已完成", "Read  src/parser.cpp / complete"));
                add(label("编辑  src/parser.cpp · 待批准", "Edit  src/parser.cpp / awaiting approval"));
                add(label("前提  文件版本 demo-ee91 · 仅此文件", "Precondition  version demo-ee91 / this file only"));
                if (details_) {
                    add("@@ -18,3 +18,3 @@ parse_config()");
                    add("  Config parse_config(std::string_view input) {");
                    add("- if (input.empty()) return ParseError{};");
                    add("+ if (input.empty()) return Config{};");
                    add("  }");
                }
                break;
            case Scene::command:
                add(label("运行  ctest · 待批准", "Run  ctest / awaiting approval"));
                add(label("目录  parser-demo · 权限  当前账户", "CWD  parser-demo / privilege  current account"));
                if (details_) {
                    add(label("参数  --test-dir build", "Args  --test-dir build"));
                    add("      --output-on-failure");
                    add("      -R");
                    add("      ^parser_empty_config$");
                }
                break;
            case Scene::failure:
                add(label("测试  parser_empty_config · 1 项失败", "Test  parser_empty_config / 1 failed"));
                add(label("记录  对应编辑版本 1", "Evidence  edit revision 1"));
                if (details_) add(label("输出  空配置仍触发解析错误。", "Output  Empty config still raises a parse error."));
                break;
            case Scene::unknown:
                add(label("运行  ctest · 已派发，结果缺失", "Run  ctest / dispatched, result missing"));
                add(label("核对  parser-demo 的文件和进程状态。", "Check files and processes in parser-demo."));
                if (details_) add(label("记录  编辑版本 1 · 自动重发 0 次", "Evidence  edit revision 1 / automatic retries 0"));
                break;
            case Scene::stopped:
            case Scene::denied:
                add(label("待批准动作没有执行。", "The pending action was never executed."));
                break;
            case Scene::recorded:
                add(label("本次演示未调用模型或工具。", "This demo did not invoke a model or tool."));
                break;
        }
    }

    void reset_decision() {
        focused_ = false;
        decision_ = {};
        notice_ = {};
    }

    void reset_review() {
        reset_decision();
        details_ = approval();
        offset_ = 0;
    }

    Scene scene_;
    bool mono_;
    bool ascii_;
    bool details_;
    bool focused_ = false;
    int offset_ = 0;
    int page_ = 1;
    std::string goal_;
    std::string_view notice_;
    Draft draft_;
    Draft decision_;
};

class Screen {
public:
    void draw(const Terminal& terminal, Size size, Frame frame) {
        const bool resized = size_ != size;
        if (!resized && rows_ == frame.rows && caret_ == frame.caret) return;
        std::string bytes = "\x1b[?25l";
        if (resized) {
            bytes += "\x1b[2J";
            rows_.clear();
        }
        for (std::size_t i = 0; i < std::max(rows_.size(), frame.rows.size()); ++i) {
            if (i < rows_.size() && i < frame.rows.size() && rows_[i] == frame.rows[i]) continue;
            bytes += "\x1b[" + std::to_string(i + 1) + ";1H\x1b[2K";
            if (i < frame.rows.size()) {
                const auto& row = frame.rows[i];
                bytes += row.style;
                bytes += row.text;
                if (!row.style.empty()) bytes += "\x1b[0m";
            }
        }
        if (frame.caret) {
            bytes += "\x1b[" + std::to_string(frame.rows.size()) + ';' + std::to_string(frame.caret) + "H\x1b[?25h";
        }
        terminal.write(bytes);
        size_ = size;
        rows_ = std::move(frame.rows);
        caret_ = frame.caret;
    }

private:
    Size size_{};
    std::vector<Row> rows_;
    int caret_ = 0;
};

class WorkspaceEditor {
public:
    WorkspaceEditor(const WorkspaceScreen& settings, WorkspaceWorker& worker)
        : settings_(settings), worker_(worker), mono_(settings.mono || std::getenv("NO_COLOR")),
          ascii_(settings.ascii) {
        draft_.text = safe_text(settings.draft, false);
        draft_.cursor = draft_.text.size();
    }

    bool refresh() {
        auto next = worker_.view();
        const auto changed = next != view_;
        if (ticket(next) != ticket(view_)) {
            reset_decision();
            offset_ = 0;
            align_ = true;
        }
        if (next.transcript != view_.transcript && follows_) {
            selected_ = next.transcript && !next.transcript->cards.empty() ? next.transcript->cards.size() - 1 : 0;
            align_ = true;
        }
        if (view_.busy && !next.busy) {
            if (!next.error.empty() && draft_.text.empty() && task_count(next) == submitted_after_) {
                draft_ = std::move(submitted_);
                allow_edit_ = submitted_allow_edit_;
            }
            submitted_ = {};
        }
        view_ = std::move(next);
        return changed;
    }

    std::size_t review_ticket() const { return ticket(view_); }
    int input_wait_ms() const { return view_.busy ? 10 : 50; }

    Frame frame(Size viewport) {
        Frame result;
        const auto width = std::clamp(viewport.columns - 1, 1, 119);
        auto& output = result.rows;
        const auto line = [&](std::string_view text, std::string_view style = {}) {
            output.push_back({wrap(text, width).front(), mono_ ? std::string_view{} : style});
        };
        line(std::string(label("▚ lay code  ·  会话 ", "[] lay code  /  Session ")) + safe_text(settings_.id, ascii_));
        if (viewport.columns < 34 || viewport.rows < 12) {
            if (viewport.rows >= 2) line(label("请放大窗口 · Esc 退出", "Enlarge window / Esc quits"));
            if (viewport.rows >= 3) line(label("Ctrl+C 停止", "Ctrl+C stops"));
            return result;
        }
        line(status(), "\x1b[36m");
        std::vector<std::string> body;
        const auto add = [&](std::string_view text) {
            while (true) {
                const auto end = text.find('\n');
                for (auto& row : wrap(text.substr(0, end), width)) body.push_back(std::move(row));
                if (end == text.npos) break;
                text.remove_prefix(end + 1);
            }
        };
        std::size_t anchor = 0;
        if (view_.review) {
            const auto& review = *view_.review;
            add(review.title);
            add(std::string(label("任务 ", "Task ")) + std::to_string(review.task) + "  /  " + review.call);
            add(review.detail);
        } else {
            add(safe_text(settings_.path, ascii_));
            if (!view_.scope.empty()) add(view_.scope);
            if (view_.transcript) {
                const auto& cards = view_.transcript->cards;
                for (std::size_t i = 0; i < cards.size(); ++i) {
                    if (i == selected_) anchor = body.size();
                    add((i == selected_ ? "> " : "  ") + cards[i].title);
                    if (expanded_ == i) add(cards[i].detail);
                }
                if (!view_.transcript->diagnostic.empty()) add(view_.transcript->diagnostic);
            }
            if (!view_.preview.empty()) {
                add(std::string(label("预览  ", "Preview  ")) + view_.preview);
            }
            if (!view_.error.empty()) add(view_.error);
        }
        page_ = std::min(viewport.rows, 48) - 8;
        const auto total = static_cast<int>(body.size());
        if (align_) {
            offset_ = follows_ && !view_.review && !expanded_ ? total - page_ :
                static_cast<int>(anchor) - 1;
            align_ = false;
        }
        offset_ = std::clamp(offset_, 0, std::max(0, total - page_));
        for (int i = offset_; i < offset_ + page_; ++i) {
            const auto text = i < total ? std::string_view(body[static_cast<std::size_t>(i)]) : std::string_view{};
            const auto style = text.starts_with("- ") ? "\x1b[31m" : text.starts_with("+ ") ? "\x1b[32m" : "";
            line(text, style);
        }
        line(std::to_string(offset_ + 1) + "-" + std::to_string(std::min(total, offset_ + page_)) + "/" +
            std::to_string(total) + std::string(label(" 行  ·  ↑↓ / PgUp PgDn", " lines  /  Up Down PgUp PgDn")));
        std::string rule;
        for (int i = 0; i < width; ++i) rule += label("─", "-");
        line(rule);
        line(label("Ctrl+O 详情 · Ctrl+C 停止 · Esc 退出", "Ctrl+O details / Ctrl+C stop / Esc quit"));
        line(instruction());
        line(notice_.empty() ? draft_label() : notice_);
        if (!view_.review || focused_) {
            auto [visible, column] = (focused_ ? decision_ : draft_).view(width - 2, ascii_);
            line("> " + visible, focused_ ? "\x1b[7m" : "");
            result.caret = 3 + column;
        } else {
            line(label("▸ 等待你的决定", "> Waiting for your decision"));
        }
        return result;
    }

    bool handle(const std::vector<Event>& batch, Size size, bool new_review) {
        const bool combined = batch.size() > 1;
        const bool compact = size.columns < 34 || size.rows < 12;
        for (const auto& event : batch) {
            const auto key = event.key;
            if (key == Key::stop) {
                if (view_.busy) {
                    worker_.stop();
                    reset_decision();
                } else {
                    notice_ = label("当前没有活动任务。", "No active task.");
                }
                return true;
            }
            if (key == Key::escape) {
                if (!focused_) return false;
                reset_decision();
                return true;
            }
            if (compact || new_review || view_.stopping) continue;
            if (key != Key::acknowledge) confirming_unknown_ = false;
            if (view_.review && key == Key::tab) {
                if (focused_) reset_decision();
                else if (!combined) {
                    focused_ = true;
                    notice_ = {};
                }
                return true;
            }
            if (focused_ && (key == Key::paste || (combined && key == Key::enter))) {
                reset_decision();
                notice_ = label("已忽略粘贴或排队回车，请重新决定。", "Queued input ignored. Use Tab again.");
                return true;
            }
            if (focused_ && key == Key::enter && view_.review) {
                worker_.decide(view_.review->ticket, decision_.text == "y" || decision_.text == "yes");
                reset_decision();
                return true;
            }
            if (key == Key::acknowledge && !combined && !view_.busy && view_.transcript &&
                (unknown() || view_.transcript->actions.close)) {
                if (confirming_unknown_) launch({unknown() ? WorkspaceAction::close_unknown : WorkspaceAction::close_task});
                else {
                    confirming_unknown_ = true;
                    notice_ = unknown() ? label("核对后再次按 F3 结清。", "Check effects. Press F3 again to close.") :
                        label("再次按 F3 结束任务并保留已保存结果。", "Press F3 again to close and keep saved results.");
                }
                return true;
            }
            if (key == Key::edit_scope && !combined && can_narrow()) {
                allow_edit_ = !allow_edit_;
                notice_ = {};
                return true;
            }
            if (key == Key::submit && !combined && !view_.busy && !unknown()) {
                if ((!view_.transcript && settings_.resume) || needs_reload()) {
                    launch({WorkspaceAction::inspect});
                } else if (view_.transcript && view_.transcript->actions.resume) {
                    launch({WorkspaceAction::resume});
                } else if ((!view_.transcript || view_.transcript->actions.follow_up) &&
                           draft_.text.find_first_not_of(' ') != draft_.text.npos) {
                    const auto action = view_.transcript ? WorkspaceAction::follow_up : WorkspaceAction::start;
                    submitted_ = std::exchange(draft_, Draft{});
                    submitted_after_ = task_count(view_);
                    submitted_allow_edit_ = allow_edit_;
                    launch({action, submitted_.text, allow_edit_});
                    allow_edit_ = true;
                }
                return true;
            }
            if (key == Key::details && !view_.review && view_.transcript && !view_.transcript->cards.empty()) {
                expanded_ = expanded_ == selected_ ? std::nullopt : std::optional{selected_};
                align_ = true;
                continue;
            }
            if (key == Key::up || key == Key::down || key == Key::first || key == Key::last ||
                key == Key::previous_page || key == Key::next_page) {
                navigate(key);
                continue;
            }
            if (!view_.review || focused_) {
                auto& draft = focused_ ? decision_ : draft_;
                if (key == Key::text && (!focused_ || draft.text.size() < 8)) draft.insert(event.text);
                if (key == Key::left || key == Key::right) draft.move(key == Key::right, false);
                if (key == Key::backspace || key == Key::erase) draft.move(key == Key::erase, true);
            }
        }
        return true;
    }

private:
    static std::size_t ticket(const WorkspaceView& view) { return view.review ? view.review->ticket : 0; }
    static std::size_t task_count(const WorkspaceView& view) { return view.transcript ? view.transcript->tasks : 0; }
    bool unknown() const { return view_.transcript && view_.transcript->actions.acknowledge_unknown; }
    bool needs_reload() const {
        if (!view_.transcript || !view_.transcript->storage_failed) return false;
        const auto actions = view_.transcript->actions;
        return view_.transcript->revision < 0 ||
            !(actions.resume || actions.close || actions.follow_up || actions.acknowledge_unknown);
    }
    bool can_narrow() const { return settings_.writable && view_.transcript && !view_.review; }
    std::string_view label(std::string_view zh, std::string_view en) const { return ascii_ ? en : zh; }

    std::string_view draft_label() const {
        if (view_.review) return label("审核当前动作", "Review current action");
        if (view_.transcript && view_.transcript->actions.close) {
            return label("F3 结束任务并保留已保存结果", "F3 closes the task and keeps saved results");
        }
        if (can_narrow()) {
            return allow_edit_ ? label("F4 禁用后续任务的编辑工具", "F4 disables edits for the next task") :
                                 label("后续任务已禁用编辑工具 · F4 切换", "Next task: edits off / F4 toggles");
        }
        return label("任务草稿", "Task draft");
    }

    std::string status() const {
        if (view_.stopping) return std::string(label("正在停止", "Stopping"));
        if (view_.review) return std::string(label("待批准 · ", "Needs approval / ")) + view_.review->title;
        if (view_.busy) {
            return std::string(view_.requesting ? label("请求中", "Requesting model") : label("工作中", "Working"));
        }
        if (!view_.error.empty()) return std::string(label("会话错误", "Session error"));
        if (!view_.transcript) return std::string(label("等待任务", "Ready for a task"));
        if (view_.transcript->storage_failed) {
            if (view_.transcript->revision < 0) {
                return std::string(label("保存失败 · 无法读取存档", "Save failed / Checkpoint unavailable"));
            }
            return std::string(label("保存失败 · 已保存版本 ", "Save failed / Saved revision ")) +
                std::to_string(view_.transcript->revision);
        }
        if (view_.transcript->stop == SessionStop::effect_unknown) {
            return std::string(view_.transcript->closed ? label("效果未知 · 已结清", "Effect unknown / Closed") :
                                                        label("效果未知 · 等待核对", "Effect unknown / Check result"));
        }
        if (view_.transcript->stop == SessionStop::cancelled) return std::string(label("已停止", "Stopped"));
        if (view_.transcript->closed && view_.transcript->stop == SessionStop::end_turn) {
            return std::string(label("已完成 · 已保存版本 ", "Complete / Saved revision ")) +
                std::to_string(view_.transcript->revision);
        }
        if (view_.transcript->closed) return std::string(label("任务已结束 · ", "Task closed / ")) +
            std::string(name(view_.transcript->stop));
        return std::string(label("任务暂停 · ", "Task paused / ")) + std::string(name(view_.transcript->stop));
    }

    std::string_view instruction() const {
        if (view_.review) return focused_ ? label("输入 y 或 yes 后回车批准。空回车拒绝。",
                                                   "y or yes + Enter approves. Enter denies.") :
                                            label("Tab 输入决定，↑↓ 翻阅动作详情。", "Tab to decide. Up/Down to review.");
        if (view_.busy) return label("可编辑下一条草稿。", "You can edit the next draft.");
        if (unknown()) return label("先核对工作区，再用 F3 结清。", "Check the workspace, then press F3.");
        if ((!view_.transcript && settings_.resume) || needs_reload()) {
            return label("F2 重新打开会话。", "F2 retries opening the session.");
        }
        if (view_.transcript && view_.transcript->actions.resume) return label("F2 恢复当前任务，保留草稿。",
                                                                       "F2 resumes the current task.");
        return label("F2 提交草稿，回车确认输入法候选。", "F2 submits draft. Enter keeps editing.");
    }

    void reset_decision() {
        focused_ = false;
        decision_ = {};
        notice_ = {};
    }

    void launch(WorkspaceCommand command) {
        worker_.launch(std::move(command));
        notice_ = {};
        follows_ = true;
        confirming_unknown_ = false;
        expanded_.reset();
        align_ = true;
    }

    void navigate(Key key) {
        if (key == Key::first || key == Key::last || key == Key::up || key == Key::down) {
            if (!view_.review && view_.transcript && !view_.transcript->cards.empty()) {
                const auto last = view_.transcript->cards.size() - 1;
                if (key == Key::first) selected_ = 0;
                else if (key == Key::last) selected_ = last;
                else if (key == Key::down) selected_ = std::min(last, selected_ + 1);
                else if (selected_) --selected_;
                follows_ = selected_ == last;
                align_ = true;
                return;
            }
            if (key == Key::first) offset_ = 0;
            else if (key == Key::last) offset_ = 1024 * 1024;
            else if (key == Key::up) --offset_;
            else ++offset_;
        } else {
            offset_ += key == Key::previous_page ? -page_ : page_;
            if (!view_.review) follows_ = false;
        }
    }

    const WorkspaceScreen& settings_;
    WorkspaceWorker& worker_;
    bool mono_;
    bool ascii_;
    WorkspaceView view_;
    bool follows_ = true;
    bool align_ = true;
    bool focused_ = false;
    bool confirming_unknown_ = false;
    bool allow_edit_ = true;
    std::size_t selected_ = 0;
    std::optional<std::size_t> expanded_;
    int offset_ = 0;
    int page_ = 1;
    std::string_view notice_;
    Draft draft_;
    Draft decision_;
    Draft submitted_;
    std::size_t submitted_after_ = 0;
    bool submitted_allow_edit_ = true;
};

} // namespace

int run(std::span<const std::string> arguments) {
    auto scene = Scene::normal;
    bool selected = false;
    bool mono = std::getenv("NO_COLOR") != nullptr;
    bool ascii = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == "--mono") {
            mono = true;
        } else if (arguments[i] == "--ascii") {
            ascii = true;
        } else if (arguments[i] == "--demo" && !selected && i + 1 < arguments.size()) {
            selected = true;
            const auto& value = arguments[++i];
            if (value == "normal") scene = Scene::normal;
            else if (value == "approval") scene = Scene::edit;
            else if (value == "failure") scene = Scene::failure;
            else if (value == "unknown") scene = Scene::unknown;
            else throw std::invalid_argument("Choose a demo: normal, approval, failure or unknown.");
        } else {
            throw std::invalid_argument("Usage: lay-code --tui [--demo normal|approval|failure|unknown] [--mono] [--ascii]");
        }
    }

    Terminal terminal;
    Demo demo(scene, mono, ascii);
    Screen screen;
    Input input;
    auto size = terminal.size();
    screen.draw(terminal, size, demo.frame(size));
    while (const auto bytes = terminal.read()) {
        const auto latest = terminal.size();
        const auto events = input.decode(*bytes);
        if (events.empty() && latest == size) continue;
        size = latest;
        if (!demo.handle(events, size)) break;
        screen.draw(terminal, size, demo.frame(size));
    }
    return 0;
}

int run_workspace(const WorkspaceScreen& settings, const WorkspaceWork& work) {
    Terminal terminal;
    WorkspaceWorker worker(work, settings.ascii);
    WorkspaceEditor editor(settings, worker);
    Screen screen;
    Input input;
    worker.launch({settings.resume ? WorkspaceAction::inspect : WorkspaceAction::prepare});
    auto size = terminal.size();
    editor.refresh();
    screen.draw(terminal, size, editor.frame(size));
    while (const auto bytes = terminal.read(editor.input_wait_ms())) {
        const auto latest = terminal.size();
        const auto events = input.decode(*bytes);
        const auto old_ticket = editor.review_ticket();
        const auto changed = editor.refresh();
        if (!changed && events.empty() && latest == size) continue;
        size = latest;
        const auto next_ticket = editor.review_ticket();
        if (!editor.handle(events, size, next_ticket && next_ticket != old_ticket)) break;
        editor.refresh();
        screen.draw(terminal, size, editor.frame(size));
    }
    return 0;
}

} // namespace lay::tui
