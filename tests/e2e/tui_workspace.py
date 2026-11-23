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
from verification import VerificationFixture


FIRST = "fix and verify"
FOLLOW_UP = "保留刚才的修改，仅运行空配置用例"
F2 = ESC + "OQ"
F3 = ESC + "OR"
F4 = ESC + "OS"


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
    line.command(data=(FIRST + "\ny\ny\ny\n").encode())
    line.command(extra=("--follow-up", FOLLOW_UP, "--no-write"), data=b"y\n")
    tui = VerificationFixture(base, "tui-equivalent", cli, driver, cmake, ctest)
    piped = subprocess.run([cli, "--tui", *tui.args], input=b"y\n", capture_output=True, timeout=5, env=tui.env)
    assert piped.returncode == 2 and b"interactive terminal" in piped.stderr and not tui.db.exists()

    with open_tui(tui) as terminal:
        terminal.expect("等待任务")
        submit(terminal, FIRST)
        terminal.expect("待批准 · 运行 ctest")
        terminal.expect("任务 1  /  verify-0")
        terminal.send("\ty\r")
        terminal.quiet()
        assert tui.count() == 0 and "> y" not in terminal.text
        terminal.key("\t", "空回车拒绝")
        terminal.key(ESC + "[200~y\r" + ESC + "[201~", "已忽略粘贴或排队回车")
        resized = len(terminal.text)
        terminal.resize(16, 48)
        terminal.expect("\x1b[16;1H", resized)
        assert tui.count() == 0
        approve(terminal)
        terminal.expect("待批准 · 编辑")
        terminal.send(ESC + "[6~")
        terminal.expect("+empty_config = allow")
        assert tui.count() == 1 and tui.file.read_bytes() == b"empty_config = reject\n"
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

    assert tui.file.read_bytes() == line.file.read_bytes() == b"empty_config = allow\n"
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
        terminal.key(F3, "核对后再次按 F3")
        terminal.key(F3, "效果未知 · 已结清")
    assert fixture.count() == 1 and fixture.saved()[0]["tasks"][-1]["closed"]


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
                terminal.send(F2)
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


if __name__ == "__main__":
    if os.name == "nt":
        from tui import PtyProcess
        if PtyProcess is None:
            sys.exit(77)
    program, driver, cmake, ctest = sys.argv[1:]
    with TemporaryDirectory(prefix="lay-workspace-tui-") as path:
        base = Path(path).resolve()
        compare(base, program, driver, cmake, ctest)
        recovery(base, program, driver, cmake, ctest)
        cancel_running(base, program, driver, cmake, ctest)
        previews(base, program)
    print("Verified TUI/CLI facts, fresh approval, cancellation, crash recovery and retry preview isolation.")
