"""Measure evidence navigation on the mixed 1/10/50/100-task history."""

import argparse
import hashlib
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import threading
import time

from mixed_history import Relay, run
from live_quality import context
from tui import ESC, PAINT, console
from tui_performance import COMPLETE, measurements, milliseconds


UP = ESC + "[A"
END = ESC + "[F"
PAGE_UP = ESC + "[5~"
PAGE_DOWN = ESC + "[6~"
EXPAND = "\x0f"
REVIEW = ESC + "[18~"
FAILURE = ESC + "[19~"
FILES = ESC + "[20~"
BACK = ESC + "[21~"
DRAFT = "return-to-this-draft"
FAILED_CALL = "call_1_run"


def paint(terminal, key):
    start = len(terminal.text)
    began = time.perf_counter_ns()
    terminal.send(key)
    # The caret sequence ends a changed frame. Search only new ConPTY output.
    terminal.expect(ESC + "[?25h", start, timeout=8)
    return milliseconds(began), terminal.text[start:]


def failing_anchor(screen):
    visible = {int(index): row for index, row in PAINT.findall(screen)}
    return any(row.startswith("> run ") and
               "[failed]" in "".join(visible.get(index + n, "") for n in range(4))
               for index, row in visible.items())


def trace(path, first):
    if not path.is_file():
        return {}
    groups = {}
    for name, _, _, ns in (line.split("\t") for line in path.read_text().splitlines()[first:]):
        if name.startswith("tui-"):
            groups.setdefault(name, []).append(round(int(ns) / 1e6, 6))
    return {name: measurements(values) for name, values in groups.items()}


def baseline(app, args, env, database, tracefile, task, server):
    saved = context(database)
    post_count = sum(map(len, server.request_sizes.values()))
    recorded = len(tracefile.read_text().splitlines()) if tracefile.is_file() else 0
    failure = next(tool for round_ in saved["rounds"] for tool in round_["tools"] if tool["id"] == FAILED_CALL)
    assert failure["verification"]["code"] != 0 and failure["run"]["edit_revision"] == 0
    moves, opens, pages = [], [], []
    with console(str(app), ["--tui", *args, "--resume", "--mono"], 16, 48, env) as terminal:
        terminal.expect(COMPLETE, timeout=8)
        terminal.key(DRAFT, "> " + DRAFT)
        terminal.quiet()
        start = len(terminal.text)
        terminal.quiet()
        assert start == len(terminal.text), "idle display repainted"
        for _ in range(len(saved["rounds"]) * 5):
            elapsed, changed = paint(terminal, UP)
            moves.append(elapsed)
            if failing_anchor(terminal.text):
                break
        else:
            raise AssertionError("The baseline did not select the failed verification")
        elapsed, changed = paint(terminal, EXPAND)
        terminal.expect("call " + FAILED_CALL, len(terminal.text) - len(changed))
        opens.append(elapsed)
        search_keys = len(moves) + 2
        for _ in range(99):
            opens.append(paint(terminal, EXPAND)[0])
        # Bottom after holding an anchor, so the first page-up always moves.
        paint(terminal, END)
        for i in range(100):
            pages.append(paint(terminal, PAGE_UP if i % 2 == 0 else PAGE_DOWN)[0])
        frame = dict(PAINT.findall(terminal.text))
        assert "> " + DRAFT in frame.values()
    assert context(database) == saved and sum(map(len, server.request_sizes.values())) == post_count
    return {
        "tasks": task, "failed_call": FAILED_CALL, "correct": True, "search_and_return_keys": search_keys,
        "extra_posts": 0, "extra_dispatches": 0, "idle_redraws": 0,
        "conpty_observed": {"navigation": measurements(moves), "page": measurements(pages), "expand": measurements(opens)},
        "host_input_to_terminal_write": trace(tracefile, recorded),
    }


def live_review(terminal, database, server):
    saved = context(database)
    assert len(saved["tasks"]) == 100
    posts = sum(map(len, server.request_sizes.values()))
    terminal.key(FAILURE, "call " + FAILED_CALL)
    terminal.key("navigation-draft", "> navigation-draft")
    terminal.key(REVIEW, "任务 100  /  call_100_edit")
    terminal.key(BACK, "call " + FAILED_CALL)
    terminal.key(FILES, "本任务文件改动 · 任务 100")
    terminal.expect("call call_100_edit")
    terminal.key(BACK, "任务 100  /  call_100_edit")
    assert context(database) == saved and sum(map(len, server.request_sizes.values())) == posts
    return {"task": 100, "approval_call": "call_100_edit", "failure_call": FAILED_CALL, "file_call": "call_100_edit",
            "matched_targets": 3, "extra_posts": 0, "extra_dispatches": 0}


def direct(app, args, env, database, tracefile, task, server):
    saved = context(database)
    posts = sum(map(len, server.request_sizes.values()))
    recorded = len(tracefile.read_text().splitlines()) if tracefile.is_file() else 0
    jumps, opens, pages, returns = [], [], [], []
    with console(str(app), ["--tui", *args, "--resume", "--mono"], 16, 48, env) as terminal:
        terminal.expect(COMPLETE, timeout=8)
        terminal.key(DRAFT, "> " + DRAFT)
        terminal.quiet()
        count = len(terminal.text)
        terminal.quiet()
        assert len(terminal.text) == count
        terminal.key(EXPAND, "Saved files")
        initial = dict(PAINT.findall(terminal.text))
        terminal.key(FILES, f"本任务文件改动 · 任务 {task}")
        terminal.expect(f"call call_{task}_edit", count)
        terminal.key(BACK, "Saved files")
        restored = dict(PAINT.findall(terminal.text))
        assert {key: initial[key] for key in map(str, range(3, 11))} == {key: restored[key] for key in map(str, range(3, 11))}
        for _ in range(100):
            jumped, screen = paint(terminal, FAILURE)
            assert "call " + FAILED_CALL in screen
            jumps.append(jumped)
            opens.append(paint(terminal, EXPAND)[0])
            returns.append(paint(terminal, BACK)[0])
        terminal.key(FAILURE, "call " + FAILED_CALL)
        for i in range(100):
            pages.append(paint(terminal, PAGE_DOWN if i % 2 == 0 else PAGE_UP)[0])
        terminal.key(BACK, "Saved files")
        assert "> " + DRAFT in dict(PAINT.findall(terminal.text)).values()
    assert context(database) == saved and sum(map(len, server.request_sizes.values())) == posts
    return {
        "tasks": task, "failed_call": FAILED_CALL, "correct": True, "search_and_return_keys": 2,
        "extra_posts": 0, "extra_dispatches": 0, "idle_redraws": 0,
        "conpty_observed": {"jump": measurements(jumps), "page": measurements(pages),
                            "expand": measurements(opens), "return": measurements(returns)},
        "host_input_to_terminal_write": trace(tracefile, recorded),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("ctest", type=Path)
    parser.add_argument("--candidate", action="store_true")
    options = parser.parse_args()
    app = options.executable.resolve(strict=True)
    with TemporaryDirectory(prefix="lay-navigation-") as directory, ThreadingHTTPServer(("127.0.0.1", 0), Relay) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            samples = run(app, options.ctest.resolve(strict=True), 1, Path(directory), server, True,
                          direct if options.candidate else baseline, live_review if options.candidate else None)
        finally:
            server.shutdown()
    print(json.dumps({"mode": "direct" if options.candidate else "sequential baseline",
                      "executable_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
                      "terminal": "Windows PyWinPTY / ConPTY", "viewport": [48, 16], "samples": samples,
                      "live_review": getattr(server, "review_navigation", None)}, indent=2))


if __name__ == "__main__":
    main()
