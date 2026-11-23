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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("ctest", type=Path)
    options = parser.parse_args()
    app = options.executable.resolve(strict=True)
    with TemporaryDirectory(prefix="lay-navigation-") as directory, ThreadingHTTPServer(("127.0.0.1", 0), Relay) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            samples = run(app, options.ctest.resolve(strict=True), 1, Path(directory), server, True, baseline)
        finally:
            server.shutdown()
    print(json.dumps({"mode": "sequential baseline", "executable_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
                      "terminal": "Windows PyWinPTY / ConPTY", "viewport": [48, 16], "samples": samples}, indent=2))


if __name__ == "__main__":
    main()
