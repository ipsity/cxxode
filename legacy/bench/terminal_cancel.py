"""Measure Ctrl+C acknowledgement and cleanup in the packaged CLI's ConPTY."""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import select
import sys
from tempfile import TemporaryDirectory
import threading
import time

from winpty import Backend, PtyProcess

from live_quality import context
from startup import memory_reader


CHILD = Path(__file__).resolve().parents[1] / "tests/fixtures/process_child.py"
ORIGINAL = b"empty_config = reject\n"


def until(predicate, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.002)
    raise TimeoutError("Fixture did not reach its expected state")


class Terminal:
    def __init__(self, args, state):
        win = Path(os.environ["SystemRoot"])
        env = {**os.environ, "LOCALAPPDATA": str(state),
               "PATH": os.pathsep.join((str(win / "System32"), str(win)))}
        self.process = PtyProcess.spawn(list(map(str, args)), env=env, dimensions=(30, 240), backend=Backend.ConPTY)
        self.text = ""

    def observe(self):
        if select.select([self.process.fileobj], [], [], 0.001)[0]:
            try:
                self.text += self.process.read(32768)
            except EOFError:
                pass

    def expect(self, marker):
        def seen():
            self.observe()
            return marker in self.text

        until(seen)
        return time.perf_counter_ns()

    def interrupt(self):
        started = time.perf_counter_ns()
        self.process.write(chr(3))
        stopping = self.expect("[session] stopping")
        self.expect("[session] cancelled")
        until(lambda: not self.process.isalive())
        return {"acknowledged_ms": round((stopping - started) / 1_000_000, 3),
                "returned_ms": round((time.perf_counter_ns() - started) / 1_000_000, 3)}


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        release = self.server.release
        self.server.requested.set()
        if self.server.ready_file is None:
            release.wait(7)
            return
        if any(item.get("type") == "function_call_output" for item in request["input"]):
            item = {"type": "message", "role": "assistant",
                    "content": [{"type": "output_text", "text": "Output captured."}]}
        else:
            item = {"type": "function_call", "id": "fc_fixture", "call_id": "call_fixture",
                    "name": "run", "arguments": json.dumps({
                        "program": sys.executable, "args": ["-u", "-B", str(CHILD), self.server.flood_mode,
                                                            str(self.server.ready_file)], "timeout_ms": 15000,
                    })}
        reply = {"type": "response.completed", "response": {"status": "completed", "output": [item]}}
        body = f"data: {json.dumps(reply)}\n\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def process_alive(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel.OpenProcess(0x00100000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error == 87:
            return False
        raise ctypes.WinError(error)
    try:
        return kernel.WaitForSingleObject(handle, 0) == 258
    finally:
        kernel.CloseHandle(handle)


def trial(app, root, config, server, phase):
    root.mkdir()
    state = root / "private"
    workspace = root / "workspace"
    workspace.mkdir()
    (workspace / "config.txt").write_bytes(ORIGINAL)
    server.requested, server.release = threading.Event(), threading.Event()
    server.flood_mode = "flood-until-cancelled"
    server.ready_file = workspace / "children.json" if phase == "output_flood" else None
    args = [app, "--workspace", workspace, "--session", phase, "--question", "fix fixture"]
    if phase == "approval":
        args.extend(["--model", "fixture-edit", "--write", "config.txt"])
    else:
        args.extend(["--provider-config", config])
        if phase == "output_flood":
            args.append("--allow-run")
    terminal = Terminal(args, state)
    try:
        if phase == "model":
            if not server.requested.wait(5):
                raise TimeoutError("Fixture did not receive the model request")
        elif phase == "approval":
            terminal.expect("Apply this edit?")
        else:
            terminal.expect("Run this command?")
            terminal.process.write("y\r")
            until(lambda: server.ready_file.is_file() and server.ready_file.stat().st_size > 0)
            children = json.loads(server.ready_file.read_text(encoding="ascii"))
            if len(children) != 2 or not all(map(process_alive, children)):
                raise RuntimeError("Expected a live leader and descendant during pipe flooding")
        result = terminal.interrupt()
        history = context(state / "LayCode" / "sessions" / f"session-{phase}.sqlite3")
        dispatched = [t for r in history["rounds"] for t in r["tools"] if t["dispatched"]]
        expected = {"model": [], "approval": ["read"], "output_flood": ["run"]}[phase]
        if terminal.process.exitstatus != 130 or [t["name"] for t in dispatched] != expected:
            raise RuntimeError("Unexpected cancellation result or dispatches")
        if phase == "approval":
            pending = history["rounds"][-1]["tools"][0]
            if pending["status"] != 0 or (workspace / "config.txt").read_bytes() != ORIGINAL:
                raise RuntimeError("Cancellation did not leave the original edit pending")
        if phase == "output_flood":
            verification = dispatched[0]["verification"]
            if ("command cancelled" not in terminal.text or any(map(process_alive, children)) or
                    not verification["stdout"]["total_bytes"] or not verification["stderr"]["total_bytes"]):
                raise RuntimeError("The cancelled process tree or its output was not fully observed")
        return result
    finally:
        server.release.set()
        terminal.process.close(force=True)


def capture_flood(app, root, config, server):
    state = root / "private"
    workspace = root / "workspace"
    workspace.mkdir(parents=True)
    server.requested, server.release = threading.Event(), threading.Event()
    server.ready_file = workspace / "flood-pid.txt"
    server.flood_mode = "flood"
    terminal = Terminal([app, "--workspace", workspace, "--session", "flood", "--provider-config", config,
                         "--allow-run", "--question", "Measure process output"], state)
    try:
        terminal.expect("Run this command?")
        private = memory_reader()
        parent = terminal.process.pid
        baseline = private(parent)
        peak_parent, peak_child = baseline, 0
        child = None
        terminal.process.write("y\r")
        deadline = time.monotonic() + 10
        # Deliberately leave the terminal unread while the application drains the child.
        while terminal.process.isalive() and time.monotonic() < deadline:
            if child is None and server.ready_file.is_file():
                content = server.ready_file.read_text(encoding="ascii")
                if content.isdecimal():
                    child = int(content)
            try:
                peak_parent = max(peak_parent, private(parent))
            except OSError:
                if terminal.process.isalive():
                    raise
            if child and process_alive(child):
                try:
                    peak_child = max(peak_child, private(child))
                except OSError:
                    if process_alive(child):
                        raise
            time.sleep(0.002)
        terminal.expect("[session] end_turn")
        until(lambda: not terminal.process.isalive())
        history = context(state / "LayCode" / "sessions" / "session-flood.sqlite3")
        tools = [t for r in history["rounds"] for t in r["tools"] if t["dispatched"]]
        if (terminal.process.exitstatus != 0 or len(tools) != 1 or not peak_child or
                tools[0]["name"] != "run" or tools[0]["verification"]["code"] != 0):
            raise RuntimeError("Flood measurement did not finish or missed the child")
        verification = tools[0]["verification"]
        outputs = {}
        for name in ("stdout", "stderr"):
            stream = verification[name]
            outputs[name] = {key: stream[key] for key in ("total_bytes", "omitted_bytes")}
            outputs[name]["retained_bytes"] = stream["total_bytes"] - stream["omitted_bytes"]
        return {"elapsed_ms": verification["elapsed_ms"], "idle_parent_private_bytes": baseline,
                "peak_parent_private_bytes": peak_parent, "additional_parent_private_bytes": peak_parent - baseline,
                "peak_child_private_bytes": peak_child, **outputs,
                "passed": peak_parent - baseline <= 16 * 1024 * 1024 and all(
                    s["total_bytes"] >= 50 * 1024 * 1024 and s["retained_bytes"] == 65536 for s in outputs.values()
                )}
    finally:
        terminal.process.close(force=True)


def measure(app, samples):
    phases = ("model", "approval", "output_flood")
    with TemporaryDirectory(prefix="lay-p4-cancel-") as directory, ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server:
        root = Path(directory)
        config = root / "providers.json"
        config.write_text(json.dumps({"default_provider": "fixture", "providers": {"fixture": {
            "protocol": "responses", "base_url": f"http://127.0.0.1:{server.server_port}",
            "model": "fixture", "api_key_env": None,
        }}}), encoding="utf-8")
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            capture = capture_flood(app, root / "capture", config, server)
            trials = {name: [trial(app, root / f"{name}-{n}", config, server, name) for n in range(samples)]
                      for name in phases}
        finally:
            server.shutdown()
            thread.join()
    return {
        "executable_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
        "terminal": "Windows ConPTY, Ctrl+C input",
        "capture": capture,
        "phases": {name: {
            "acknowledged_p95_ms": sorted(row["acknowledged_ms"] for row in rows)[math.ceil(0.95 * samples) - 1],
            "max_returned_ms": max(row["returned_ms"] for row in rows),
            "passed": sorted(row["acknowledged_ms"] for row in rows)[math.ceil(0.95 * samples) - 1] <= 100
                      and max(row["returned_ms"] for row in rows) <= 3000,
            "samples": rows,
        } for name, rows in trials.items()},
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--samples", type=int, default=20)
    arguments = parser.parse_args()
    if arguments.samples < 1:
        parser.error("Choose at least one sample.")
    result = measure(arguments.executable.resolve(strict=True), arguments.samples)
    print(json.dumps(result, indent=2))
    sys.exit(0 if result["capture"]["passed"] and all(phase["passed"] for phase in result["phases"].values()) else 1)
