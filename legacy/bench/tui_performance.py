"""Measure the packaged workspace TUI on Windows ConPTY."""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
from http.server import ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
from tempfile import TemporaryDirectory
import threading
import time

from live_quality import context
from startup import memory_reader
from stream_preview import Fixture as PreviewFixture, MARKER
from terminal_cancel import Fixture as ProcessFixture, Terminal, process_alive, until


F2 = "\x1bOQ"
READY = "等待任务"
COMPLETE = "已完成"


def milliseconds(start, end=None):
    return round(((end if end is not None else time.perf_counter_ns()) - start) / 1e6, 3)


def measurements(rows):
    return {"count": len(rows), "median_ms": statistics.median(rows),
            "p95_ms": sorted(rows)[math.ceil(len(rows) * 0.95) - 1], "samples_ms": rows}


def process_clock():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    fields = [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, *fields]
    kernel.GetSystemTimePreciseAsFileTime.argtypes = fields[:1]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]

    def ticks(filetime):
        return (filetime.dwHighDateTime << 32) | filetime.dwLowDateTime

    def now():
        data = wintypes.FILETIME()
        kernel.GetSystemTimePreciseAsFileTime(ctypes.byref(data))
        return ticks(data)

    def creation(pid):
        process = kernel.OpenProcess(0x1000, False, pid)
        if not process:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            data = [wintypes.FILETIME() for _ in fields]
            if not kernel.GetProcessTimes(process, *(ctypes.byref(value) for value in data)):
                raise ctypes.WinError(ctypes.get_last_error())
            return ticks(data[0])
        finally:
            kernel.CloseHandle(process)
    return now, creation


def commands(app, workspace, name, config=None, *extra):
    return [app, "--tui", "--mono", "--workspace", workspace, "--session", name,
            *(["--provider-config", config] if config else ["--model", "fixture-edit", "--write", "config.txt"]),
            *extra]


def close(terminal):
    try:
        if terminal.process.isalive():
            terminal.process.write("\x1b")
            until(lambda: not terminal.process.isalive())
        if terminal.process.exitstatus != 0:
            raise RuntimeError(f"TUI quit with {terminal.process.exitstatus}")
    finally:
        terminal.process.close(force=True)


def submit(terminal, text):
    terminal.process.write(text)
    terminal.expect("> " + text)
    terminal.process.write(F2)


def startup(app, root, state):
    private = memory_reader()
    now, creation = process_clock()
    warm, terminal_setup, keys, memory = [], [], [], []
    for i in range(101):
        begin = time.perf_counter_ns()
        terminal = Terminal(commands(app, root, "startup"), state)
        try:
            combined = milliseconds(begin, terminal.expect(READY))
            shown = now()
            elapsed = round((shown - creation(terminal.process.pid)) / 1e4, 3)
            resident = private(terminal.process.pid)
            begin = time.perf_counter_ns()
            terminal.process.write("sample")
            keystroke = milliseconds(begin, terminal.expect("> sample"))
            if i:
                warm.append(elapsed)
                terminal_setup.append(combined)
                keys.append(keystroke)
                memory.append(resident)
            else:
                first = {"ms": elapsed, "including_conpty_setup_ms": combined, "private_bytes": resident}
        finally:
            close(terminal)
    return {"first": first, "warm": measurements(warm),
            "including_conpty_setup": measurements(terminal_setup), "draft_input": measurements(keys),
            "max_idle_private_bytes": max(memory)}


def configure(base, server, filename):
    config = base / filename
    config.write_text(json.dumps({"default_provider": "local", "providers": {"local": {
        "protocol": "responses", "base_url": f"http://127.0.0.1:{server.server_port}",
        "model": "fixture", "api_key_env": None,
    }}}), encoding="utf-8")
    return config


def previews(app, root, state):
    times = []
    with ThreadingHTTPServer(("127.0.0.1", 0), PreviewFixture) as server:
        config = configure(root, server, "preview.json")
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            for index in range(20):
                terminal = Terminal(commands(app, root, f"preview-{index}", config), state)
                try:
                    terminal.expect(READY)
                    submit(terminal, "Measure model preview")
                    seen = terminal.expect("预览  " + MARKER)
                    times.append(milliseconds(server.sent_ns, seen))
                    terminal.expect(COMPLETE)
                finally:
                    close(terminal)
        finally:
            server.shutdown()
    return measurements(times)


def reset_process_fixture(server, mode, marker):
    server.requested = threading.Event()
    server.release = threading.Event()
    server.flood_mode = mode
    server.ready_file = marker


def decide(terminal, approve):
    terminal.expect("Tab 输入决定")
    terminal.process.write("\t")
    terminal.expect("空回车拒绝")
    if approve:
        terminal.process.write("y")
        terminal.expect("> y")
    terminal.process.write("\r")


def running_cancellations(app, root, state, server, config):
    feedback, cleanup = [], []
    for index in range(20):
        marker = root / f"cancel-{index}.json"
        reset_process_fixture(server, "flood-until-cancelled", marker)
        terminal = Terminal(commands(app, root, f"cancel-{index}", config, "--allow-run"), state)
        try:
            terminal.expect(READY)
            submit(terminal, "Cancel a running fixture command")
            decide(terminal, True)
            until(lambda: marker.is_file() and marker.stat().st_size > 0)
            children = json.loads(marker.read_text(encoding="ascii"))
            assert len(children) == 2 and all(map(process_alive, children))
            started = time.perf_counter_ns()
            terminal.process.write("\x03")
            until(lambda: terminal.observe() or "正在停止" in terminal.text or "已停止" in terminal.text)
            feedback.append(milliseconds(started))
            until(lambda: not any(map(process_alive, children)))
            cleanup.append(milliseconds(started))
            terminal.expect("已停止")
        finally:
            close(terminal)
    return {"cancel_feedback": measurements(feedback), "child_cleanup_observed": measurements(cleanup)}


def process_trials(app, root, state, *, capture_stress=True):
    private = memory_reader()
    with ThreadingHTTPServer(("127.0.0.1", 0), ProcessFixture) as server:
        config = configure(root, server, "process.json")
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            if not capture_stress:
                return running_cancellations(app, root, state, server, config)
            marker = root / "capture-pid.txt"
            reset_process_fixture(server, "flood", marker)
            terminal = Terminal(commands(app, root, "capture", config, "--allow-run"), state)
            try:
                terminal.expect(READY)
                submit(terminal, "Capture output without consuming the terminal")
                terminal.expect("Tab 输入决定")
                idle = peak = private(terminal.process.pid)
                decide(terminal, True)
                until(lambda: marker.is_file() and marker.stat().st_size > 0)
                db = state / "LayCode/sessions/session-capture.sqlite3"
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and not context(db).get("finished"):
                    peak = max(peak, private(terminal.process.pid))
                    time.sleep(0.005)
                terminal.expect(COMPLETE)
                history = context(db)
                tools = [tool for round_ in history["rounds"] for tool in round_["tools"] if tool["name"] == "run"]
                assert len(tools) == 1 and tools[0]["verification"]["code"] == 0
                result = tools[0]["verification"]
                streams = {}
                for name in ("stdout", "stderr"):
                    stream = result[name]
                    streams[name] = {"total_bytes": stream["total_bytes"],
                                     "retained_bytes": stream["total_bytes"] - stream["omitted_bytes"]}
                    assert streams[name]["total_bytes"] >= 50 * 1024 * 1024 and streams[name]["retained_bytes"] == 65536
                capture = {"elapsed_ms": result["elapsed_ms"], "idle_private_bytes": idle,
                           "peak_private_bytes": peak, "additional_private_bytes": peak - idle, **streams}
            finally:
                close(terminal)
            return {"capture": capture, **running_cancellations(app, root, state, server, config)}
        finally:
            server.release.set()
            server.shutdown()


def history_sample(app, root, state):
    env = {**os.environ, "LOCALAPPDATA": str(state)}
    db = state / "LayCode/sessions/session-history.sqlite3"
    terminal = Terminal(commands(app, root, "history"), state)
    try:
        terminal.expect(READY)
        submit(terminal, "Read the fixture")
        decide(terminal, False)
        terminal.expect(COMPLETE)
    finally:
        close(terminal)
    args = [app, "--workspace", root, "--session", "history", "--model", "fixture-edit", "--write", "config.txt"]
    private = memory_reader()
    samples = {}
    for target in (1, 41):
        while (count := len(context(db)["tasks"])) < target:
            completed = subprocess.run([*args, "--follow-up", f"Re-read the fixture {count}", "--no-write"],
                                       stdin=subprocess.DEVNULL, capture_output=True, timeout=10, env=env)
            assert completed.returncode == 0, completed.stderr
        delays, memory = [], []
        for _ in range(10):
            start = time.perf_counter_ns()
            terminal = Terminal(commands(app, root, "history", None, "--resume"), state)
            try:
                delays.append(milliseconds(start, terminal.expect(COMPLETE)))
                memory.append(private(terminal.process.pid))
            finally:
                close(terminal)
        samples[str(target)] = {**measurements(delays), "database_bytes": db.stat().st_size,
                                "max_idle_private_bytes": max(memory)}
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--interaction-gates", action="store_true",
                        help="Measure startup, input, preview and running-command cancellation without the standalone flood capture.")
    args = parser.parse_args()
    app = args.executable.resolve(strict=True)
    with TemporaryDirectory(prefix="lay-p7-benchmark-") as directory:
        base = Path(directory)
        state, root = base / "private", base / "workspace"
        root.mkdir()
        (root / "config.txt").write_text("empty_config = reject\n", encoding="utf-8")
        result = {"executable_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
                  "terminal": "Windows ConPTY", "startup": startup(app, root, state),
                  "complete_preview_event_to_terminal": previews(app, root, state),
                  "process": process_trials(app, root, state, capture_stress=not args.interaction_gates),
                  "saved_history": history_sample(app, root, state)}
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
