#include "tui.hpp"
#include "tui_terminal.hpp"
#include "tui_text.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace lay::tui {
namespace {

enum class Key {
    text, enter, escape, tab, stop, details, submit, up, down, left, right, first, last,
    previous_page, next_page, backspace, erase, paste
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

} // namespace lay::tui
