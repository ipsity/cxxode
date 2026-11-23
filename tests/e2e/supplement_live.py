"""Drive P16 in the real workspace TUI with ordered loopback and SQLite gates."""

import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import sys
from tempfile import TemporaryDirectory
import threading
import time

from editing import Fixture
from supplement import INPUT_ID, SUPPLEMENT, kill_at
from tui import ESC, console
from tui_workspace import F2, F3, F5, approve, submit
from two_file_editing import OLD_SOURCE, NEW_SOURCE, OLD_TEST, NEW_TEST, SOURCE, TEST, changes

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "contract"))
import providers as responses
import readonly_contract as anthropic


F6 = ESC + "[17~"
FINAL = "CORRECTION FINISHED"
HEADING = f"[Task supplement {INPUT_ID}]\n{SUPPLEMENT}"
SUPERSEDED = f"superseded by {INPUT_ID}"
OLD_READS = [("old-a", "read", {"path": "a.txt"}), ("old-b", "read", {"path": "b.txt"})]
NEW_READS = [("new-a", "read", {"path": "a.txt"}), ("new-b", "read", {"path": "b.txt"})]


def tool_reply(protocol, calls):
    if protocol == "anthropic":
        return anthropic.START + b"".join(anthropic.tool(i, *call) for i, call in enumerate(calls)) + anthropic.tail("tool_use")
    if protocol == "chat-completions":
        return responses.chat({"tool_calls": [responses.call(i, name, tool, json.dumps(args))
                                               for i, (name, tool, args) in enumerate(calls)]}) + responses.chat(
            {}, "tool_calls") + responses.sse("[DONE]")
    return responses.completed([{"type": "function_call", "id": f"fc_{name}", "call_id": name,
                                 "name": tool, "arguments": json.dumps(args)} for name, tool, args in calls])


def answer(protocol, text=FINAL):
    if protocol == "anthropic":
        return anthropic.answer(text)
    if protocol == "chat-completions":
        return responses.chat({"content": text}, "stop") + responses.sse("[DONE]")
    return responses.completed([responses.message(text)])


def half_tool(protocol):
    if protocol == "anthropic":
        return anthropic.START + anthropic.text(0, "old partial") + anthropic.tool_start(1, "old-a", "read") + (
            anthropic.part(1, '{"path"'))
    if protocol == "chat-completions":
        return responses.chat({"content": "old partial"}) + responses.chat(
            {"tool_calls": [responses.call(0, "old-a", "read", '{"path"')]})
    item = {"type": "function_call", "id": "fc_old-a", "call_id": "old-a", "name": "read", "arguments": ""}
    return responses.response("response.output_text.delta", delta="old partial") + responses.response(
        "response.output_item.added", output_index=0, item=item) + responses.response(
        "response.function_call_arguments.delta", output_index=0, delta='{"path"')


def history(payload, protocol):
    return payload["input" if protocol == "responses" else "messages"]


def results(items, protocol):
    if protocol == "anthropic":
        return {part["tool_use_id"]: part["content"] for item in items if item.get("role") == "user"
                for part in item["content"] if isinstance(part, dict) and part["type"] == "tool_result"}
    if protocol == "chat-completions":
        return {item["tool_call_id"]: item["content"] for item in items if item.get("role") == "tool"}
    return {item["call_id"]: item["output"] for item in items if item.get("type") == "function_call_output"}


def assert_order(items, protocol, expected, updated=()):
    supplement, = [i for i, item in enumerate(items) if item.get("role") == "user" and item.get("content") == HEADING]
    for segment, ids in ((items[:supplement], expected), (items[supplement + 1:], updated)):
        observed = results(segment, protocol)
        assert list(observed) == list(ids), (protocol, observed, ids)
        if protocol == "anthropic":
            actual = [part["id"] for item in segment if item.get("role") == "assistant"
                      for part in item["content"] if part["type"] == "tool_use"]
        elif protocol == "chat-completions":
            actual = [call["id"] for item in segment for call in item.get("tool_calls", [])]
        else:
            actual = [item["call_id"] for item in segment if item.get("type") == "function_call"]
        assert actual == list(ids), (protocol, actual, ids)


class Relay(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        server = self.server
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        index = len(server.requests)
        server.requests.append(raw)
        try:
            body, code, partial = server.plan(index, json.loads(raw))
            if partial == b"":
                server.arrived.set()
                assert server.release.wait(10), "first-byte gate was not released"
            self.send_response(code)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body) + len(partial or b"")))
            self.end_headers()
            if partial:
                self.wfile.write(partial)
                self.wfile.flush()
                server.arrived.set()
                assert server.release.wait(10), "partial tool gate was not released"
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        except Exception as error:
            server.errors.append(repr(error))
            self.close_connection = True


def send(body, code=200, partial=None):
    return body, code, partial


@contextmanager
def relay(base, name, cli, driver, protocol, plan, evidence):
    fixture = Fixture(base, name, cli, driver, data=None)
    for path in ("a.txt", "b.txt"):
        (fixture.root / path).write_text(f"{path} evidence", encoding="utf-8")
    config = base / f"{name}.json"
    with ThreadingHTTPServer(("127.0.0.1", 0), Relay) as server:
        server.requests, server.errors, server.plan = [], [], plan
        server.arrived, server.release = threading.Event(), threading.Event()
        config.write_text(json.dumps({"default_provider": "local", "providers": {"local": {
            "protocol": protocol, "model": "fixture", "api_key_env": None,
            "base_url": f"http://127.0.0.1:{server.server_port}/v1",
        }}}), encoding="utf-8")
        fixture.args = ["--workspace", str(fixture.root), "--session", name, "--provider-config", str(config)]
        if protocol == "anthropic":
            fixture.args += ["--provider", "anthropic", "--model", "fixture"]
            fixture.env["LAY_TEST_ANTHROPIC_BASE"] = f"http://127.0.0.1:{server.server_port}/v1"
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield fixture, server
        finally:
            server.release.set()
            server.shutdown()
            thread.join()
        assert not server.errors, server.errors
        if evidence is not None:
            snapshot, phases = fixture.saved()
            evidence.append({
                "case": name,
                "protocol": protocol,
                "draft_text": SUPPLEMENT,
                "tasks": [{"id": task["id"], "question": task["question"], "closed": task["closed"],
                           "supplement": task.get("supplement")} for task in snapshot["tasks"]],
                "checkpoints": [{"revision": revision, "phase": phase} for revision, phase in enumerate(phases)],
                "model_posts_seen": len(server.requests),
                "supplement_copies_per_post": [
                    sum(item.get("content") == HEADING for item in history(json.loads(raw), protocol))
                    for raw in server.requests
                ],
                "requests_in_snapshot": snapshot["requests"],
                "dispatches_in_snapshot": snapshot["dispatches"],
                "tools": [{"id": tool["id"], "tool": tool["name"], "dispatched": tool["dispatched"], "status": tool["status"],
                           "verified_exit": tool.get("verification", {}).get("code")}
                          for round in snapshot["rounds"] for tool in round["tools"]],
                "final_file_bytes_hex": {file.relative_to(fixture.root).as_posix(): file.read_bytes().hex()
                                         for file in sorted(fixture.root.rglob("*")) if file.is_file()},
            })


def terminal(fixture, *options):
    return console(fixture.driver, ["--tui", *fixture.args, "--mono", *options], 28, 100, fixture.env)


def stage_supplement(screen):
    screen.key(F6, "F2 提交补充")
    screen.key(SUPPLEMENT, "> " + SUPPLEMENT)


def wait(predicate, message):
    end = time.monotonic() + 7
    while not predicate() and time.monotonic() < end:
        time.sleep(0.02)
    assert predicate(), message


class Gate:
    def __init__(self, fixture, point):
        base = fixture.root.parent / "worker-gate"
        self.entered = base.with_suffix(".ready")
        self.release = base.with_suffix(".release")
        fixture.env.update(LAY_TEST_GATE_POINT=point, LAY_TEST_GATE=str(base))

    def wait(self):
        wait(self.entered.is_file, "worker did not enter the SQLite checkpoint gate")

    def open(self):
        self.release.touch()


def saved_input(fixture, *, before_round, dispatches, requests):
    snapshot, phases = fixture.saved()
    task, = snapshot["tasks"]
    item = task["supplement"]
    assert (task["id"], task["first_request"], task["first_dispatch"]) == (1, 0, 0)
    assert (item["id"], item["task_id"], item["text"], item["before_round"]) == (INPUT_ID, 1, SUPPLEMENT, before_round)
    assert phases.count("supplement") == 1 and item["saved_revision"] == phases.index("supplement")
    assert phases.count("supplement_included") == (before_round is not None)
    assert snapshot["dispatches"] == dispatches and snapshot["requests"] == requests
    return snapshot


def stream_boundaries(base, cli, driver, evidence=None):
    for protocol in ("responses", "chat-completions", "anthropic"):
        boundaries = ("first-byte", "partial", "round")
        if protocol == "responses":
            boundaries += ("first-read",)
        for boundary in boundaries:
            old = ("old-a", "old-b") if boundary in ("round", "first-read") else ()
            kept = ("old-a",) if boundary == "first-read" else ()
            skipped = old[len(kept):]
            retry = protocol == "responses" and boundary == "partial"

            def plan(index, payload):
                items = history(payload, protocol)
                if index == 0:
                    assert all(item.get("content") != HEADING for item in items)
                    if boundary == "partial":
                        return send(b"\n", partial=half_tool(protocol))
                    return send(tool_reply(protocol, OLD_READS), partial=b"" if boundary == "first-byte" else None)
                if index == 1 and retry:
                    assert_order(items, protocol, old)
                    return send(b"", 429)
                if index == 1 + retry:
                    assert_order(items, protocol, old)
                    observed = results(items, protocol)
                    assert all(SUPERSEDED in observed[name] for name in skipped)
                    assert all("evidence" in observed[name] for name in kept)
                    return send(tool_reply(protocol, NEW_READS))
                assert index == 2 + retry
                assert_order(items, protocol, old, ("new-a", "new-b"))
                assert all("evidence" in results(items, protocol)[name] for name in ("new-a", "new-b"))
                return send(answer(protocol))

            with relay(base, f"{protocol}-{boundary}", cli, driver, protocol, plan, evidence) as (fixture, server):
                gate_point = {"round": "after_read_reply", "first-read": "after_read_dispatch"}.get(boundary)
                gate = Gate(fixture, gate_point) if gate_point else None
                try:
                    with terminal(fixture) as screen:
                        screen.expect("等待任务")
                        submit(screen, "Observe two files")
                        screen.expect("F6 补充本任务")
                        if gate:
                            gate.wait()
                        else:
                            assert server.arrived.wait(5)
                        if boundary == "partial":
                            screen.expect("old partial")
                        stage_supplement(screen)
                        screen.send(F2)
                        if gate:
                            screen.expect("补充等待保存")
                            gate.open()
                        screen.expect(FINAL, timeout=7)
                        screen.expect("补充已纳入请求")
                finally:
                    if gate:
                        gate.open()
                    server.release.set()
                snapshot = saved_input(fixture, before_round=int(bool(old)), dispatches=2 + len(kept), requests=3 + retry)
                assert len(server.requests) == 3 + retry
                if retry:
                    assert server.requests[1] == server.requests[2]
                records = [tool for round in snapshot["rounds"] for tool in round["tools"]]
                assert [tool["id"] for tool in records if tool["dispatched"]] == [*kept, "new-a", "new-b"]
                assert [tool["id"] for tool in records if tool["result"] == SUPERSEDED] == list(skipped)


def approval_demo(base, cli, driver, evidence=None):
    protocol = "responses"
    read_files = [("read-source", "read", {"path": SOURCE}), ("read-test", "read", {"path": TEST})]

    def plan(index, payload):
        items = history(payload, protocol)
        observed = results(items, protocol)
        if index == 0:
            return send(tool_reply(protocol, read_files))
        if index == 1:
            version = lambda key: re.search(r"^version=(\S+)$", observed[key], re.M)[1]
            return send(tool_reply(protocol, [
                ("edit-source", "edit", {"path": SOURCE, "version": version("read-source"),
                                         "old_text": OLD_SOURCE, "new_text": NEW_SOURCE}),
                ("edit-test", "edit", {"path": TEST, "version": version("read-test"),
                                       "old_text": OLD_TEST, "new_text": NEW_TEST}),
            ]))
        prior = ("read-source", "read-test", "edit-source", "edit-test")
        assert observed["edit-source"].startswith("applied ") and SUPERSEDED in observed["edit-test"]
        if index == 2:
            assert_order(items, protocol, prior)
            return send(tool_reply(protocol, [("verify-existing", "run", {"program": sys.executable, "args": ["-B", TEST]})]))
        assert index == 3
        assert_order(items, protocol, prior, ("verify-existing",))
        assert "command exited 0" in observed["verify-existing"]
        assert "Observed at workspace edit revision 1." in observed["verify-existing"]
        return send(answer(protocol))

    with relay(base, "approval-demo", cli, driver, protocol, plan, evidence) as (fixture, server):
        fixture.args += ["--write", SOURCE, "--write", TEST, "--allow-run"]
        for path, data in ((SOURCE, OLD_SOURCE), (TEST, OLD_TEST)):
            file = fixture.root / path
            file.parent.mkdir()
            file.write_bytes(data.encode())
        with terminal(fixture) as screen:
            screen.expect("等待任务")
            submit(screen, "Fix pagination and add tests")
            screen.expect("任务 1  /  edit-source")
            approve(screen)
            screen.expect("任务 1  /  edit-test")
            assert changes(fixture) == [NEW_SOURCE, OLD_TEST]
            stage_supplement(screen)
            screen.send(F2)
            screen.expect("任务 1  /  verify-existing")
            screen.send("y\r")
            screen.quiet()
            screen.key("\t", "空回车拒绝")
            screen.key(ESC + "[200~yes\r\n" + ESC + "[201~", "已忽略粘贴或排队回车")
            screen.resize(16, 48)
            screen.expect("任务 1  /  verify-existing")
            assert fixture.saved()[0]["dispatches"] == 3
            approve(screen)
            screen.expect(FINAL)
        assert changes(fixture) == [NEW_SOURCE, OLD_TEST]
        snapshot = saved_input(fixture, before_round=2, dispatches=4, requests=4)
        kept, skipped = snapshot["rounds"][1]["tools"]
        assert kept["edit"]["approved"] is True and kept["dispatched"]
        assert not skipped["dispatched"] and skipped["edit"].get("approved") is not True
        assert skipped["result"] == SUPERSEDED
        run = snapshot["rounds"][2]["tools"][0]
        assert run["run"]["edit_revision"] == 1 and run["verification"]["code"] == 0
        closed = fixture.saved()
        listing = fixture.command(resume=True, extra=("--inspect",), data=b"", driver=False)
        assert f"[supplement {INPUT_ID}]" in listing and "included before round 3" in listing
        assert fixture.saved() == closed and len(server.requests) == 4


def dispatched_command(base, cli, driver, evidence=None):
    protocol = "responses"
    command = "from pathlib import Path; Path('executions.txt').open('a').write('ran\\n')"

    def plan(index, payload):
        items = history(payload, protocol)
        if index == 0:
            return send(tool_reply(protocol, [
                ("old-command", "run", {"program": sys.executable, "args": ["-c", command]}), *OLD_READS,
            ]))
        assert index == 1
        assert_order(items, protocol, ("old-command", "old-a", "old-b"))
        observed = results(items, protocol)
        assert "command exited 0" in observed["old-command"] and all(SUPERSEDED in observed[tool] for tool in ("old-a", "old-b"))
        return send(answer(protocol))

    with relay(base, "command-first", cli, driver, protocol, plan, evidence) as (fixture, server):
        fixture.args.append("--allow-run")
        # The checkpoint labels a tool batch by its last proposal.
        gate = Gate(fixture, "after_read_dispatch")
        with terminal(fixture) as screen:
            screen.expect("等待任务")
            submit(screen, "Run a check, then read files")
            screen.expect("任务 1  /  old-command")
            approve(screen)
            gate.wait()
            stage_supplement(screen)
            screen.key(F2, "补充等待保存")
            screen.key("NEXT DRAFT", "> NEXT DRAFT")
            screen.key(F2, "草稿未发送")
            assert not (fixture.root / "executions.txt").exists()
            gate.open()
            screen.expect(FINAL)
            screen.expect("> NEXT DRAFT")
        assert (fixture.root / "executions.txt").read_text() == "ran\n"
        snap = saved_input(fixture, before_round=1, dispatches=1, requests=2)
        assert snap["rounds"][0]["tools"][0]["verification"]["code"] == 0
        assert len(server.requests) == 2


def stop_and_terminal_order(base, cli, driver, evidence=None):
    protocol = "responses"
    for winner in ("stop", "supplement", "terminal"):
        def plan(index, payload):
            if index == 0:
                return send(answer(protocol, "Original task ended"), partial=None if winner == "terminal" else b"")
            assert index == 1 and winner == "supplement"
            assert_order(history(payload, protocol), protocol, ())
            return send(answer(protocol))

        with relay(base, f"{winner}-first", cli, driver, protocol, plan, evidence) as (fixture, server):
            gate = None
            if winner != "stop":
                gate = Gate(fixture, "after_reply" if winner == "terminal" else "before_supplement")
            with terminal(fixture) as screen:
                screen.expect("等待任务")
                submit(screen, "Order the task boundary")
                screen.expect("F6 补充本任务")
                if winner == "terminal":
                    gate.wait()
                else:
                    assert server.arrived.wait(5)
                stage_supplement(screen)
                if winner == "stop":
                    screen.send("\x03")
                    screen.expect("已停止")
                    screen.key(F2, "补充保留在草稿区")
                    screen.expect("> " + SUPPLEMENT)
                else:
                    screen.key(F2, "补充等待保存")
                    gate.wait()
                    if winner == "supplement":
                        screen.key("\x03", "正在停止")
                    gate.open()
                    if winner == "supplement":
                        screen.expect("任务暂停 · 补充待处理")
                    else:
                        screen.expect("补充已退回草稿")
                        screen.expect("> " + SUPPLEMENT)
                        screen.expect("已完成")
            assert len(server.requests) == 1
            server.release.set()
            if winner == "supplement":
                saved_input(fixture, before_round=None, dispatches=0, requests=1)
                kill_at(fixture, "after_supplement_included", extra=("--resume",),
                        inspect=lambda: saved_input(fixture, before_round=0, dispatches=0, requests=1))
                assert len(server.requests) == 1
                with terminal(fixture, "--resume") as screen:
                    screen.expect("补充已纳入请求")
                    screen.send(F5)
                    screen.expect(FINAL)
                saved_input(fixture, before_round=0, dispatches=0, requests=2)
                assert len(server.requests) == 2
            else:
                snapshot, phases = fixture.saved()
                assert "supplement" not in snapshot["tasks"][0] and "supplement" not in phases
                assert snapshot["tasks"][0]["closed"] and snapshot["dispatches"] == 0


def lost_checkpoint(base, cli, driver, evidence=None):
    protocol = "responses"
    for point in ("supplement-save", "command-result"):
        def plan(index, _):
            assert index == 0
            if point == "supplement-save":
                return send(answer(protocol), partial=b"")
            return send(tool_reply(protocol, [
                ("once", "run", {"program": sys.executable, "args": [
                    "-c", "from pathlib import Path; Path('executions.txt').open('a').write('once\\n')",
                ]}),
            ]))

        with relay(base, point, cli, driver, protocol, plan, evidence) as (fixture, server):
            gate = None
            if point == "supplement-save":
                fixture.env["LAY_TEST_FULL"] = "before_supplement"
            else:
                fixture.args.append("--allow-run")
                gate = Gate(fixture, "before_run_result")
            with terminal(fixture) as screen:
                screen.expect("等待任务")
                submit(screen, "Check recovery")
                screen.expect("F6 补充本任务")
                if gate:
                    screen.expect("任务 1  /  once")
                    approve(screen)
                    gate.wait()
                    assert (fixture.root / "executions.txt").read_text() == "once\n"
                else:
                    assert server.arrived.wait(5)
                stage_supplement(screen)
                if gate:
                    screen.key(F2, "补充等待保存")
                    screen.force_exit()
                    gate.open()
                else:
                    screen.send(F2)
                    screen.expect("保存失败")
                    screen.expect("补充已退回草稿")
                    screen.expect("> " + SUPPLEMENT)
                    assert "补充已保存" not in screen.text
            snapshot, phases = fixture.saved()
            assert "supplement" not in snapshot["tasks"][0] and "supplement" not in phases
            if gate:
                with terminal(fixture, "--resume") as screen:
                    screen.expect("效果未知 · 等待核对")
                    screen.key(F3, "再次按 F3 结清")
                    screen.key(F3, "效果未知 · 已结清")
                assert (fixture.root / "executions.txt").read_text() == "once\n"
                assert fixture.saved()[1].count("dispatch") == 1
            assert len(server.requests) == 1


if __name__ == "__main__":
    if os.name == "nt":
        from tui import PtyProcess
        if PtyProcess is None:
            sys.exit(77)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cli")
    parser.add_argument("driver")
    parser.add_argument("--trace", type=Path)
    args = parser.parse_args()
    evidence = [] if args.trace else None
    with TemporaryDirectory(prefix="lay-p16-live-") as path:
        workspace = Path(path)
        for scenario in (stream_boundaries, approval_demo, dispatched_command, stop_and_terminal_order, lost_checkpoint):
            scenario(workspace, args.cli, args.driver, evidence)
    if args.trace:
        args.trace.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Verified P16 stream boundaries, approval invalidation, tool results, stop ordering and terminal draft return.")
