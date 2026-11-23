"""Exercise two exact edit paths with real approval, testing, and recovery."""

from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading

from editing import Fixture, finish, wait_for
from tui import ESC, console


SOURCE = "src/pager.py"
TEST = "tests/test_paging.py"
OTHER = "unapproved.txt"
OLD_SOURCE = "def page(items, number, size):\n    start = number * size\n    return items[start:start + size]\n"
NEW_SOURCE = OLD_SOURCE.replace("number * size", "(number - 1) * size")
OLD_TEST = (
    "from pathlib import Path\n"
    "import sys\n"
    "sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))\n"
    "from pager import page\n"
    "assert page([], 1, 2) == []\n"
)
CHECKS = "assert page([1, 2, 3], 1, 2) == [1, 2]\nassert page([1, 2, 3], 2, 2) == [3]\n"
NEW_TEST = OLD_TEST + CHECKS
QUESTION = "修复从 1 开始的分页，并为首页和不足一页的末页补上回归测试。"


def call(id_, name, **arguments):
    return {"type": "function_call", "id": "fc_" + id_, "call_id": id_,
            "name": name, "arguments": json.dumps(arguments)}


class Relay(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        results = {item["call_id"]: item["output"] for item in request["input"]
                   if item.get("type") == "function_call_output"}
        self.server.requests += 1
        if "read-source" not in results:
            items = [call("read-source", "read", path=SOURCE), call("read-test", "read", path=TEST)]
        elif "edit-source" not in results:
            def version(id_):
                return re.search(r"^version=(\S+)$", results[id_], re.M)[1]

            items = [
                call("edit-source", "edit", path=SOURCE, version=version("read-source"),
                     old_text="number * size", new_text="(number - 1) * size"),
                call("edit-test", "edit", path=TEST, version=version("read-test"),
                     old_text="assert page([], 1, 2) == []\n", new_text="assert page([], 1, 2) == []\n" + CHECKS),
            ]
        elif results.get("edit-test", "").startswith("applied ") and "outside-edit" not in results:
            items = [call("outside-edit", "edit", path=OTHER, version="absent", new_text="out of scope\n")]
        elif "outside-edit" in results and "run-test" not in results:
            items = [call("run-test", "run", program=sys.executable, args=["-B", TEST])]
        else:
            items = [{"type": "message", "id": "msg_pager", "role": "assistant",
                      "content": [{"type": "output_text", "text": "The recorded edits and verification are shown above."}]}]
        event = {"type": "response.completed", "response": {"status": "completed", "output": items}}
        data = f"data: {json.dumps(event)}\n\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


@contextmanager
def pager(base, name, cli, driver):
    fixture = Fixture(base, name, cli, driver, data=None)
    for path, content in ((SOURCE, OLD_SOURCE), (TEST, OLD_TEST)):
        target = fixture.root / path
        target.parent.mkdir()
        target.write_bytes(content.encode())
    profile = base / f"{name}.json"
    with ThreadingHTTPServer(("127.0.0.1", 0), Relay) as server:
        server.requests = 0
        profile.write_text(json.dumps({"default_provider": "pager", "providers": {"pager": {
            "protocol": "responses", "model": "paging-fixture", "api_key_env": None,
            "base_url": f"http://127.0.0.1:{server.server_port}/v1",
        }}}))
        fixture.args = ["--workspace", str(fixture.root), "--session", name, "--provider-config", str(profile),
                        "--write", TEST, "--write", SOURCE, "--allow-run"]
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            yield fixture, server
        finally:
            server.shutdown()
            worker.join()


def changes(fixture):
    return [(fixture.root / path).read_bytes().decode() for path in (SOURCE, TEST)]


def approvals(snapshot):
    return [row for round_ in snapshot["rounds"] for row in round_["tools"] if row["name"] == "edit"]


def tui(fixture, *flags):
    return console(fixture.cli, ["--tui", *fixture.args, "--mono", *flags], 27, 100, fixture.env)


def decide(terminal):
    terminal.key("\t", "空回车拒绝")
    terminal.key("y", "> y")
    terminal.send("\r")


def assert_reopen(fixture, server, second_state, status=0, reason=""):
    requests = server.requests
    first = f"[edit progress] {SOURCE} [applied]"
    second = f"[edit progress] {TEST} [{second_state}]"
    with tui(fixture, "--resume") as terminal:
        terminal.expect(first)
        terminal.expect(second)
        if reason:
            terminal.key("\x0f", reason)
        if second_state == "applied":
            terminal.send(ESC + "[H")
            terminal.quiet()
            terminal.send((ESC + "[B") * 4)
            terminal.quiet()
            terminal.key("\x0f", "+assert page([1, 2, 3], 2, 2) == [3]")
            terminal.expect("approval granted")
    snapshot, _ = fixture.saved()
    idx = fixture.args.index("--write")
    fixture.args[idx + 1], fixture.args[idx + 3] = fixture.args[idx + 3], fixture.args[idx + 1]
    output = fixture.command(status, driver=False, resume=True, data=b"")
    assert first in output and second in output and reason in output, output
    assert "[edit evidence] " + SOURCE in output and "approval granted" in output
    assert "Apply this edit?" not in output
    assert server.requests == requests
    source, test, *_ = approvals(fixture.saved()[0])
    assert source["dispatched"] and source["status"] == 1
    assert sum(record["dispatched"] for record in approvals(snapshot)) == int(test["dispatched"]) + 1
    assert fixture.saved()[0]["dispatches"] == snapshot["dispatches"]
    assert not (fixture.root / OTHER).exists()


def normal(base, cli, driver):
    with pager(base, "pager-normal", cli, driver) as (fixture, server):
        with tui(fixture) as terminal:
            terminal.expect("等待任务")
            terminal.key(QUESTION, "> " + QUESTION)
            terminal.send(ESC + "OQ")
            terminal.expect(f"待批准 · 编辑 {SOURCE}")
            terminal.expect("+    start = (number - 1) * size")
            decide(terminal)
            terminal.expect(f"待批准 · 编辑 {TEST}")
            assert changes(fixture) == [NEW_SOURCE, OLD_TEST]
            terminal.expect("+assert page([1, 2, 3], 2, 2) == [3]")
            decide(terminal)
            terminal.expect("run-test")
            assert changes(fixture) == [NEW_SOURCE, NEW_TEST]
            decide(terminal)
            terminal.expect("已完成")
        saved, _ = fixture.saved()
        a, b, unauthorized = approvals(saved)
        assert a["edit"]["path"] == SOURCE and b["edit"]["path"] == TEST
        assert all(row["status"] == 1 and row["dispatched"] for row in (a, b))
        assert a["edit"]["before"] != b["edit"]["before"]
        assert a["edit"]["approved"] and b["edit"]["approved"]
        assert "+    start = (number - 1) * size" in a["edit"]["diff"]
        assert "+assert page([1, 2, 3], 2, 2) == [3]" in b["edit"]["diff"]
        assert not unauthorized["dispatched"] and "outside" in unauthorized["result"]
        result = saved["rounds"][-2]["tools"][0]
        assert result["verification"]["code"] == 0 and result["run"]["edit_revision"] == 2
        assert_reopen(fixture, server, "applied")
        assert server.requests == 5
        idx = fixture.args.index("--write")
        original = fixture.args[idx + 3]
        fixture.args[idx + 3] = "another.py"
        assert "session scope changed" in fixture.command(2, driver=False, resume=True, data=b"")
        fixture.args[idx + 3] = fixture.args[idx + 1]
        assert "must be distinct" in fixture.command(2, driver=False, resume=True, data=b"")
        fixture.args[idx + 3] = original
        assert "excessive" in fixture.command(2, driver=False, resume=True, extra=("--write", OTHER), data=b"")
        assert "workspace-relative file" in fixture.command(2, driver=False, resume=True,
                                                             extra=("--exclude", "tests"), data=b"")
        assert server.requests == 5

    old = base / "unfixed"
    for path, content in ((SOURCE, OLD_SOURCE), (TEST, NEW_TEST)):
        target = old / path
        target.parent.mkdir(parents=True)
        target.write_bytes(content.encode())
    result = subprocess.run([sys.executable, "-B", TEST], cwd=old, capture_output=True, timeout=5)
    assert result.returncode == 1 and b"AssertionError" in result.stderr


def refused_and_conflict(base, cli, driver):
    for conflict in (False, True):
        name = "pager-conflict" if conflict else "pager-denied"
        with pager(base, name, cli, driver) as (fixture, server):
            process = fixture.paused("unused", data=(QUESTION + "\n").encode())
            original = OLD_TEST
            try:
                wait_for(process, b"Apply this edit? [y/N] ")
                process.stdin.write(b"y\n")
                process.stdin.flush()
                wait_for(process, b"Apply this edit? [y/N] ")
                assert changes(fixture) == [NEW_SOURCE, OLD_TEST]
                if conflict:
                    original += "# external change\n"
                    (fixture.root / TEST).write_bytes(original.encode())
                reason = "file changed during approval" if conflict else "edit denied by user"
                code, output = finish(process, b"y\n" if conflict else b"n\n")
                assert code == 0 and reason in output, output
            finally:
                if process.poll() is None:
                    finish(process, kill=True)
            assert changes(fixture) == [NEW_SOURCE, original]
            assert_reopen(fixture, server, "not started", reason=reason)
            second = approvals(fixture.saved()[0])[1]
            assert second["edit"]["path"] == TEST and not second["dispatched"]
            assert second["edit"]["approved"] is conflict and "+assert page(" in second["edit"]["diff"]


def interrupted(base, cli, driver):
    with pager(base, "pager-killed", cli, driver) as (fixture, server):
        process = fixture.paused("after_edit_result", data=(QUESTION + "\ny\n").encode())
        try:
            wait_for(process, b"[checkpoint] after_edit_result")
            first, second = approvals(fixture.saved()[0])
            assert first["dispatched"] and first["status"] == 1 and not second["dispatched"]
        finally:
            finish(process, kill=True)
        assert changes(fixture) == [NEW_SOURCE, OLD_TEST]
        before = (fixture.root / SOURCE).stat()
        assert_reopen(fixture, server, "not started", status=1)
        assert server.requests == 2

        process = fixture.paused("before_edit_result", data=b"y\n", extra=("--resume",))
        try:
            wait_for(process, b"[checkpoint] before_edit_result")
            second = approvals(fixture.saved()[0])[1]
            assert second["dispatched"] and second["status"] == 0
        finally:
            finish(process, kill=True)
        assert changes(fixture) == [NEW_SOURCE, NEW_TEST]
        assert_reopen(fixture, server, "effect unknown", status=1, reason="observed bytes match expected after state")
        assert server.requests == 2
        saved, checkpoints = fixture.saved()
        assert not saved["finished"] and approvals(saved)[1]["status"] == 4
        assert checkpoints.count("dispatch") == 4
        after = (fixture.root / SOURCE).stat()
        assert (before.st_ino, before.st_mtime_ns) == (after.st_ino, after.st_mtime_ns)


if __name__ == "__main__":
    if os.name == "nt":
        from tui import PtyProcess
        if PtyProcess is None:
            sys.exit(77)
    with TemporaryDirectory(prefix="lay-two-files-") as temp:
        base = Path(temp)
        normal(base, *sys.argv[1:])
        refused_and_conflict(base, *sys.argv[1:])
        interrupted(base, *sys.argv[1:])
    print("Verified independent edits, matching regression test, and shared partial recovery.")
