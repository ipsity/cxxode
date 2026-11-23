"""Compare the real workspace TUI and the line CLI using isolated fixtures."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading
import time

from tui import ESC, PAINT, console
from editing import finish
from verification import VerificationFixture


FIRST = "修复并验证空配置"
FOLLOW_UP = "保留刚才的修改，仅运行空配置用例"
F2 = ESC + "OQ"
F3 = ESC + "OR"
F4 = ESC + "OS"
F5 = ESC + "[15~"


def approve(terminal):
    terminal.key("\t", "空回车拒绝")
    terminal.key("y", "> y")
    terminal.send("\r")


def submit(terminal, text):
    terminal.key(text, "> " + text)
    terminal.send(F2)


def open_tui(fixture, *extra):
    return console(fixture.cli, ["--tui", *fixture.args, "--mono", *extra], 24, 80, fixture.env)


def facts(snapshot):
    tasks = [(item["id"], item["question"], item["first_round"], item["end_round"], item["stop"], item["allow_edit"])
             for item in snapshot["tasks"]]
    rounds = []
    for item in snapshot["rounds"]:
        tools = []
        for tool in item["tools"]:
            run, verification = tool.get("run", {}), tool.get("verification", {})
            edit = tool["edit"] or {}
            tools.append((tool["id"], tool["name"], tool["dispatched"], tool["status"],
                          [(key, kind, value) for key, kind, value in tool["arguments"] if key != "version"],
                          run.get("arguments"), run.get("edit_revision"), verification.get("code"),
                          verification.get("test_summary"), edit.get("before_digest"), edit.get("after_digest")))
        rounds.append((item["text"], tools))
    return (tasks, rounds, snapshot["requests"], snapshot["dispatches"])


def compare(base, cli, driver, cmake, ctest):
    line = VerificationFixture(base, "line-equivalent", cli, driver, cmake, ctest)
    ending = b" # " + b"x" * 800 + b" LONG-DIFF-END\n"
    original = b"empty_config = reject" + ending
    revised = b"empty_config = allow" + ending
    line.file.write_bytes(original)
    line.command(data=(FIRST + "\ny\ny\ny\n").encode())
    line.command(extra=("--follow-up", FOLLOW_UP, "--no-write"), data=b"y\n")
    tui = VerificationFixture(base, "tui-equivalent", cli, driver, cmake, ctest)
    tui.file.write_bytes(original)
    piped = subprocess.run([cli, "--tui", *tui.args], input=b"y\n", capture_output=True, timeout=5, env=tui.env)
    assert piped.returncode == 2 and b"interactive terminal" in piped.stderr and not tui.db.exists()

    with open_tui(tui) as terminal:
        terminal.expect("等待任务")
        terminal.key(ESC + "[200~" + "第一行\r\n第二行" + ESC + "[201~", "第二行")
        terminal.send("\r")
        terminal.quiet()
        assert not tui.db.exists() and tui.count() == 0
        terminal.send("\x7f" * len("第一行  第二行"))
        terminal.key(FIRST, "> " + FIRST)
        terminal.send("\r")
        terminal.quiet()
        assert not tui.db.exists() and tui.count() == 0
        terminal.send(F2)
        terminal.expect("待批准 · 运行 ctest")
        terminal.expect("任务 1  /  verify-0")
        terminal.key(F2, "请先完成当前审批")
        assert tui.count() == 0
        terminal.send("\ty\r")
        terminal.quiet()
        assert tui.count() == 0 and "> y" not in terminal.text
        terminal.key("\t", "空回车拒绝")
        terminal.key(ESC + "[200~yes\r\n第二行\r\n" + ESC + "[201~", "已忽略粘贴或排队回车")
        terminal.key("\t", "空回车拒绝")
        terminal.key("y", "> y")
        terminal.resize(10, 30)
        terminal.expect("请放大窗口")
        terminal.send("\ty\r")
        terminal.quiet()
        assert tui.count() == 0
        resized = len(terminal.text)
        terminal.resize(16, 48)
        terminal.expect("\x1b[16;1H", resized)
        terminal.expect("任务 1  /  verify-0", resized)
        terminal.send("\r")
        terminal.quiet()
        assert tui.count() == 0
        approve(terminal)
        terminal.expect("待批准 · 编辑")
        terminal.key(ESC + "[F", "LONG-DIFF-END")
        terminal.send((ESC + "[5~") * 2)
        terminal.expect("+empty_config = allow")
        terminal.key(ESC + "[H", "任务 1  /  edit-2")
        resized = len(terminal.text)
        terminal.resize(24, 80)
        terminal.expect("任务 1  /  edit-2", resized)
        assert tui.count() == 1 and tui.file.read_bytes() == original
        approve(terminal)
        terminal.expect("任务 1  /  verify-3")
        approve(terminal)
        terminal.expect("已完成")
        assert tui.count() == 2
        terminal.send(ESC + "[A")
        terminal.quiet()
        terminal.key("\x0f", "executable ")
        terminal.send(ESC + "[6~")
        terminal.expect("argument[0]")
        terminal.expect("edit revision 1")
        terminal.send("\x0f")
        terminal.quiet()
        start = len(terminal.text)
        terminal.key(F4, "后续任务已禁用编辑工具")
        submit(terminal, FOLLOW_UP)
        terminal.expect("任务 2  /  follow-up-2", start)
        approve(terminal)
        terminal.expect("已完成", start)

    assert tui.file.read_bytes() == line.file.read_bytes() == revised
    assert tui.count() == line.count() == 3
    snapshot, phases = tui.saved()
    assert facts(snapshot) == facts(line.saved()[0])
    assert phases == line.saved()[1]
    with open_tui(tui, "--resume") as terminal:
        terminal.expect("已完成")
        terminal.expect("#2 你  " + FOLLOW_UP)
    assert tui.saved() == (snapshot, phases) and tui.count() == 3

    with open_tui(tui, "--resume") as terminal:
        terminal.expect("已完成")
        submit(terminal, "取消这个测试")
        terminal.expect("任务 3  /  follow-up-3")
        terminal.key("\x03", "已停止")
        assert tui.count() == 3
        cancelled, _ = tui.saved()
        assert cancelled["tasks"][-1]["closed"] and cancelled["dispatches"] == snapshot["dispatches"]
        terminal.send("\r")
        terminal.quiet()
        submit(terminal, "重新运行同一用例")
        terminal.expect("任务 4  /  follow-up-4")
        approve(terminal)
        terminal.expect("已完成", terminal.text.index("任务 4"))
    assert tui.count() == 4 and tui.saved()[0]["tasks"][-1]["closed"]


def recovery(base, cli, driver, cmake, ctest):
    fixture = VerificationFixture(base, "tui-kill", cli, driver, cmake, ctest, hold=True)
    line = VerificationFixture(base, "line-kill", cli, driver, cmake, ctest, hold=True)
    process = line.paused("unused", data=(FIRST + "\ny\n").encode())
    try:
        deadline = time.monotonic() + 3
        while not line.count() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert line.count() == 1
    finally:
        finish(process, kill=True)
    assert "effect_unknown" in line.command(1, resume=True, data=b"")
    line.command(resume=True, extra=("--accept-unknown", "--close-task"), data=b"")
    line.command(extra=("--follow-up", FOLLOW_UP, "--no-write"), data=b"y\n")
    with open_tui(fixture) as terminal:
        terminal.expect("等待任务")
        submit(terminal, FIRST)
        terminal.expect("待批准 · 运行 ctest")
        approve(terminal)
        deadline = time.monotonic() + 3
        while not fixture.count() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert fixture.count() == 1 and terminal.alive()
        terminal.force_exit()
    pending, _ = fixture.saved()
    assert pending["rounds"][-1]["tools"][0]["dispatched"] and pending["dispatches"] == 1
    with open_tui(fixture, "--resume") as terminal:
        terminal.expect("效果未知 · 等待核对")
        terminal.expect("effect unknown")
        assert fixture.count() == 1 and fixture.saved()[0]["dispatches"] == 1
        terminal.key("x", "> x")
        waiting = fixture.saved()
        terminal.key(F2, "草稿未发送 · 先核对未知效果")
        assert fixture.saved() == waiting
        terminal.key("\x7f", "> ")
        terminal.key(F3, "核对后再次按 F3")
        terminal.key(F3, "效果未知 · 已结清")
        terminal.key(F4, "后续任务已禁用编辑工具")
        start = len(terminal.text)
        submit(terminal, FOLLOW_UP)
        terminal.expect("任务 2  /  follow-up-2", start)
        approve(terminal)
        terminal.expect("已完成", start, timeout=8)
    assert fixture.count() == line.count() == 2
    assert fixture.file.read_bytes() == line.file.read_bytes()
    assert facts(fixture.saved()[0]) == facts(line.saved()[0])


def other_pairs(base, cli, driver, cmake, ctest):
    changed = b"empty_config = reject\n# externally modified between tasks\n"
    for scenario in ("external", "denied"):
        denied = scenario == "denied"
        tui = VerificationFixture(base, "tui-" + scenario, cli, driver, cmake, ctest)
        line = VerificationFixture(base, "line-" + scenario, cli, driver, cmake, ctest)
        line.command(data=(FIRST + ("\nn\ny\nn\n" if denied else "\ny\ny\ny\n")).encode())
        if not denied:
            line.file.write_bytes(changed)
        line.command(extra=("--follow-up", FOLLOW_UP, "--no-write"), data=b"" if denied else b"y\n")
        with open_tui(tui) as terminal:
            terminal.expect("等待任务")
            submit(terminal, FIRST)
            terminal.expect("任务 1  /  verify-0")
            if denied:
                terminal.key("\t", "空回车拒绝")
                terminal.send("\r")
            else:
                approve(terminal)
            terminal.expect("任务 1  /  edit-2")
            approve(terminal)
            terminal.expect("任务 1  /  verify-3")
            if denied:
                terminal.key("\t", "空回车拒绝")
                terminal.send("\r")
            else:
                approve(terminal)
            terminal.expect("已完成")
            if not denied:
                tui.file.write_bytes(changed)
            terminal.key(F4, "后续任务已禁用编辑工具")
            start = len(terminal.text)
            submit(terminal, FOLLOW_UP)
            if not denied:
                terminal.expect("任务 2  /  follow-up-2", start)
                approve(terminal)
            terminal.expect("已完成", start)
        assert tui.file.read_bytes() == line.file.read_bytes()
        assert tui.count() == line.count() == (0 if denied else 3)
        snapshot, phases = tui.saved()
        assert facts(snapshot) == facts(line.saved()[0]) and phases == line.saved()[1]
        final_tool = snapshot["rounds"][-2]["tools"][0]
        if denied:
            assert "command denied by user" in final_tool["result"] and not final_tool["dispatched"]
        else:
            assert "CTest reported 1 failed" in final_tool["result"]


def cancel_running(base, cli, driver, cmake, ctest):
    fixture = VerificationFixture(base, "tui-cancel-running", cli, driver, cmake, ctest, hold=True)
    with open_tui(fixture) as terminal:
        terminal.expect("等待任务")
        submit(terminal, FIRST)
        terminal.expect("待批准 · 运行 ctest")
        approve(terminal)
        deadline = time.monotonic() + 3
        while not fixture.count() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert fixture.count() == 1
        terminal.key("\x03", "已停止")
        terminal.expect("command cancelled")
    snapshot, _ = fixture.saved()
    assert snapshot["tasks"][-1]["closed"] and snapshot["dispatches"] == 1
    assert "command cancelled" in snapshot["rounds"][-1]["tools"][0]["result"]
    assert not (fixture.root / "observed-completion.txt").exists()
    with open_tui(fixture, "--resume") as terminal:
        terminal.expect("已停止")
        terminal.expect("command cancelled")
        submit(terminal, "检查上次取消后保留的记录")
        terminal.expect("任务 2  /  follow-up-2")
        terminal.key("\t", "空回车拒绝")
        terminal.send("\r")
        terminal.expect("已完成")
    recovered, _ = fixture.saved()
    assert recovered["dispatches"] == 1 and fixture.count() == 1
    assert recovered["rounds"][:len(snapshot["rounds"])] == snapshot["rounds"]

    exit_pending = VerificationFixture(base, "tui-exit-pending", cli, driver, cmake, ctest)
    with open_tui(exit_pending) as terminal:
        terminal.expect("等待任务")
        submit(terminal, FIRST)
        terminal.expect("待批准 · 运行 ctest")
    snapshot, _ = exit_pending.saved()
    assert snapshot["tasks"][-1]["closed"] and snapshot["dispatches"] == 0
    assert exit_pending.count() == 0


class Relay(ThreadingHTTPServer):
    def shutdown_request(self, request):
        self.close_request(request)


class RetryFixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        attempt = self.server.attempts
        self.server.attempts += 1
        previews = ["PREVIEW_FIRST", "PREVIEW_SECOND"]
        preview = {"type": "response.output_text.delta", "delta": previews[attempt]}
        complete = {"type": "response.completed", "response": {"status": "completed", "output": [{
            "id": "msg_final", "type": "message", "role": "assistant",
            "content": [{"type": "output_text", "text": "COMMITTED_RESULT \x1b]52;c;IGNORED\x07"}],
        }]}}
        stream = lambda message: f"data: {json.dumps(message)}\n\n".encode()
        prefix, suffix = stream(preview), stream(complete)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(prefix) + len(suffix)))
        self.end_headers()
        self.wfile.write(prefix)
        self.wfile.flush()
        self.server.release[attempt].wait(5)
        if attempt == 0:
            linger = struct.pack("hh" if os.name == "nt" else "ii", 1, 0)
            self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        else:
            self.wfile.write(suffix)


def previews(base, cli):
    root = base / "retry-workspace"
    root.mkdir()
    profile = base / "retry-providers.json"
    environment = {**os.environ, "LAY_TEST_RELAY_KEY": "fixture-only",
                   "LOCALAPPDATA": str(base / "relay-state"), "XDG_STATE_HOME": str(base / "relay-state")}
    with Relay(("127.0.0.1", 0), RetryFixture) as server:
        server.attempts = 0
        server.release = (threading.Event(), threading.Event())
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        profile.write_text(json.dumps({"default_provider": "relay", "providers": {"relay": {
            "protocol": "responses", "model": "fixture-model", "api_key_env": "LAY_TEST_RELAY_KEY",
            "base_url": f"http://127.0.0.1:{server.server_port}/v1",
        }}}))
        args = ["--tui", "--workspace", str(root), "--session", "retry", "--provider-config", str(profile), "--mono"]
        try:
            with console(cli, args, 24, 80, environment) as terminal:
                terminal.expect("等待任务")
                submit(terminal, "Test the model retry")
                terminal.expect("PREVIEW_FIRST")
                terminal.key("next draft", "> next draft")
                terminal.key(F2, "草稿未发送，可以继续编辑")
                terminal.quiet()
                assert server.attempts == 1
                server.release[0].set()
                terminal.expect("PREVIEW_SECOND")
                screen = dict(PAINT.findall(terminal.text))
                assert not any("PREVIEW_FIRST" in row for row in screen.values())
                server.release[1].set()
                terminal.expect("COMMITTED_RESULT")
                terminal.expect("已完成")
                assert ESC + "]52;" not in terminal.text
                screen = dict(PAINT.findall(terminal.text))
                assert not any("PREVIEW_" in row for row in screen.values())
                assert any(row == "> next draft" for row in screen.values())
                assert server.attempts == 2
        finally:
            for event in server.release:
                event.set()
            server.shutdown()


class EvidenceFixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        index = self.server.requests
        self.server.requests += 1
        path = self.server.filename
        if index < 2:
            action, args = ("run", {"program": "ctest", "args": ["-R", "^empty_config$|no match"]}) if index == 0 else (
                "read", {"path": path})
            item = {"type": "function_call", "id": f"fc_evidence_{index}", "call_id": f"call_evidence_{index}",
                    "name": action, "arguments": json.dumps(args, ensure_ascii=False)}
        else:
            item = {"type": "message", "id": "msg_evidence", "role": "assistant",
                    "content": [{"type": "output_text", "text": f"配置依据是 `{path}:1`。"}]}
        payload = {"type": "response.completed", "response": {"status": "completed", "output": [item]}}
        encoded = f"data: {json.dumps(payload)}\n\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


def task_closing(base, cli, driver):
    from editing import Fixture, NEW, OLD, edit_record, wait_for

    paused = Fixture(base, "offline-close-approval", cli, driver)
    paused.command(1, driver=False)
    before, _ = paused.saved()
    with open_tui(paused, "--resume") as terminal:
        terminal.expect("任务暂停 · approval_required")
        terminal.expect("F3 结束任务")
        terminal.key(F3, "再次按 F3 结束任务")
        terminal.key(F3, "任务已结束")
    saved, phases = paused.saved()
    assert saved["finished"] and saved["requests"] == before["requests"]
    assert saved["dispatches"] == before["dispatches"] and paused.file.read_bytes() == OLD
    assert edit_record(saved)["result"] == "task closed by user" and phases[-1] == "closed_task"

    fault = Fixture(base, "final-reply-full", cli, driver)
    args = ["--tui", *fault.args, "--mono"]
    env = {**fault.env, "LAY_TEST_FULL": "before_reply"}
    with console(driver, args, 27, 95, env) as terminal:
        terminal.expect("等待任务")
        submit(terminal, "fix fixture")
        terminal.expect("待批准 · 编辑")
        approve(terminal)
        terminal.expect("保存失败 · 已保存版本")
        terminal.expect("回复尚未保存 · The host recorded the result above.")
        assert "已完成" not in terminal.text
        saved, phases = fault.saved()
        assert not saved["finished"] and phases[-1] == "request"
        assert fault.file.read_bytes() == NEW and saved["dispatches"] == 2
        assert "The host recorded the result above." not in [row["text"] for row in saved["rounds"]]
        terminal.expect("F3 结束任务")
        terminal.key("new draft", "> new draft")
        terminal.expect("F5 继续原任务")
        unsaved = fault.saved()
        terminal.key(F2, "草稿未发送 · F5 继续原任务")
        assert fault.saved() == unsaved
        start = len(terminal.text)
        terminal.send(F5)
        terminal.expect("保存失败 · 已保存版本", start)
        assert len(fault.saved()[0]["tasks"]) == 1 and fault.saved()[0]["dispatches"] == 2
    assert "not closed" in fault.command(2, driver=False, extra=("--follow-up", "new task"), data=b"")
    saved, _ = fault.saved()
    fault.command(driver=False, resume=True, extra=("--close-task",), data=b"")
    closed, _ = fault.saved()
    assert closed["finished"] and closed["requests"] == saved["requests"]
    assert closed["dispatches"] == saved["dispatches"]
    fault.command(driver=False, extra=("--follow-up", "new task", "--no-write"), data=b"")
    assert len(fault.saved()[0]["tasks"]) == 2

    unknown = Fixture(base, "generic-close-unknown", cli, driver)
    process = unknown.paused("after_edit_dispatch")
    try:
        wait_for(process, b"[checkpoint] after_edit_dispatch")
    finally:
        finish(process, kill=True)
    saved, _ = unknown.saved()
    blocked = unknown.command(2, driver=False, resume=True, extra=("--close-task",), data=b"")
    assert "Review any unknown effects first" in blocked
    recovered, _ = unknown.saved()
    assert recovered["dispatches"] == saved["dispatches"] and recovered["requests"] == saved["requests"]
    assert edit_record(recovered)["status"] == 4 and unknown.file.read_bytes() == OLD


def shared_evidence(base, cli, driver, cmake, ctest):
    fixture = VerificationFixture(base, "evidence", cli, driver, cmake, ctest)
    fixture.file.write_bytes(b"empty_config = allow\n")
    fixture.args[fixture.args.index("fixture-verify")] = "fixture-evidence"
    config = base / "evidence-provider.json"
    with ThreadingHTTPServer(("127.0.0.1", 0), EvidenceFixture) as server:
        server.requests = 0
        server.filename = fixture.file.name
        config.write_text(json.dumps({"default_provider": "relay", "providers": {"relay": {
            "model": "fixture-evidence", "protocol": "responses", "api_key_env": None,
            "base_url": f"http://127.0.0.1:{server.server_port}/v1",
        }}}))
        fixture.args += ["--provider-config", str(config)]
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with console(cli, ["--tui", *fixture.args, "--mono"], 16, 48, fixture.env) as terminal:
                terminal.expect("等待任务")
                submit(terminal, "检查配置并引用出处")
                terminal.expect("任务 1  /  call_evidence_0")
                terminal.key(ESC + "[F", '参数 1 "^empty_config$|no match"')
                terminal.expect("输出摘录将发送至 http://127.0.0.1")
                assert fixture.count() == 0 and server.requests == 1
                terminal.key(ESC + "[H", "任务 1  /  call_evidence_0")
                approve(terminal)
                terminal.expect("已完成")
            saved = fixture.saved()
            run, read = [row["tools"][0] for row in saved[0]["rounds"][:2]]
            observed = run["verification"]["observed_utc"]
            fixture.file.write_bytes(b"empty_config = reject\n# externally changed\n")
            line = fixture.command(resume=True, driver=False, data=b"")
            reference = f"[historical citation] {fixture.file.name}:1 task=1 round=2"
            version = read["version"]
            summary = "CTest reported 1 passing test(s)."
            assert reference + f" version={version}" in line
            assert "command exited 0 at edit revision 0" in line and observed in line and summary in line
            assert "Current workspace verification: unknown." in line
            with console(cli, ["--tui", *fixture.args, "--resume", "--mono"], 32, 155, fixture.env) as terminal:
                terminal.expect("已完成")
                terminal.expect(reference + f" version={version[:24]}")
                terminal.expect(read["observed_utc"])
                terminal.send(ESC + "[H")
                terminal.quiet()
                terminal.send(ESC + "[B")
                terminal.quiet()
                terminal.key("\x0f", "command exited 0 at edit revision 0")
                terminal.expect("observed " + observed)
                terminal.expect(summary)
                terminal.expect("Current workspace verification: unknown.")
            assert fixture.saved() == saved and fixture.count() == 1 and server.requests == 3
        finally:
            server.shutdown()


def draft_input(base, cli, driver):
    from editing import Fixture

    fixture = Fixture(base, "draft-input", cli, driver)
    with open_tui(fixture) as terminal:
        terminal.expect("等待任务")
        terminal.key(F2, "请先输入任务草稿")
        terminal.send(ESC + "[200~line1\nline2" + ESC + "[201~")
        terminal.expect("line2")
        if os.name != "nt":
            terminal.expect("> line1 line2")
            terminal.expect("粘贴的换行和制表符已转为空格")
        assert not fixture.db.exists()
    with open_tui(fixture, "--ascii") as terminal:
        terminal.expect("Ready for a task")
        terminal.key("a" * 4094 + "中bbignored", "Draft limit: 4096 bytes")
        terminal.quiet()
        assert not fixture.db.exists()
        terminal.send(F2)
        terminal.expect("Needs approval / Edit")
        assert terminal.text.isascii()
        saved, _ = fixture.saved()
        assert saved["tasks"][0]["question"] == "a" * 4094 + "bb"
        assert saved["requests"] == 2 and saved["dispatches"] == 1


if __name__ == "__main__":
    if os.name == "nt":
        from tui import PtyProcess
        if PtyProcess is None:
            sys.exit(77)
    program, driver, cmake, ctest = sys.argv[1:]
    with TemporaryDirectory(prefix="lay-workspace-tui-") as path:
        base = Path(path).resolve()
        compare(base, program, driver, cmake, ctest)
        other_pairs(base, program, driver, cmake, ctest)
        recovery(base, program, driver, cmake, ctest)
        cancel_running(base, program, driver, cmake, ctest)
        task_closing(base, program, driver)
        draft_input(base, program, driver)
        previews(base, program)
        shared_evidence(base, program, driver, cmake, ctest)
    print("Verified TUI/CLI facts, fresh approval, cancellation, crash recovery and retry preview isolation.")
