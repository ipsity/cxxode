"""Measure a mixed 100-task history with a fixed loopback Responses relay."""

import argparse
from collections import Counter
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading
import time

from live_quality import context
from tui_performance import COMPLETE, READY, configure, measurements, milliseconds

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests/e2e"))
from tui import ESC, console


TARGETS = (1, 10, 50, 100)
CONFIG = "config.txt"
SEED = "seed.txt"
REFERENCE = "seed.txt:1"
CTEST_ARGS = ["--output-on-failure", "-R", "empty config"]


class Relay(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        data = self.rfile.read(int(self.headers["Content-Length"]))
        items = json.loads(data)["input"]
        tasks = [item["content"] for item in items if item.get("role") == "user"
                 and isinstance(item.get("content"), str) and item["content"].startswith("Mixed task ")]
        task = int(tasks[-1].split()[2]) if tasks else 1
        calls = {item["call_id"]: item["output"] for item in items if item.get("type") == "function_call_output"}
        self.server.request_sizes.setdefault(task, []).append(len(data))

        def call(step, tool, arguments):
            return {"type": "function_call", "id": f"fc_{task}_{step}", "call_id": f"call_{task}_{step}",
                    "name": tool, "arguments": json.dumps(arguments)}

        seed = calls.get("call_1_seed")
        read = calls.get(f"call_{task}_read")
        if task == 1 and not seed:
            item = call("seed", "read", {"path": SEED})
        elif not read:
            item = call("read", "read", {"path": CONFIG})
        elif task % 10 in (1, 2) and f"call_{task}_edit" not in calls:
            version = re.search(r"^version=(\S+)$", read, re.M)[1]
            old_text = re.search(r"^1: (value=.*)$", read, re.M)[1]
            item = call("edit", "edit", {"path": CONFIG, "version": version,
                                        "old_text": old_text, "new_text": f"value={task:03d}"})
        elif task % 10 == 2 and f"call_{task}_run" not in calls:
            item = call("run", "run", {"program": self.server.ctest, "args": CTEST_ARGS})
        else:
            answer = f"Task {task} done."
            if task in TARGETS:
                summary = "\n".join(part["text"] for item in items if item.get("role") == "user"
                                    and isinstance(item.get("content"), list) for part in item["content"])
                observed = re.search(r"\(read seed\.txt\): observation recorded, version=(\S+)", summary)
                version = (re.search(r"^version=(\S+)$", seed, re.M)[1] if seed else
                           observed[1] if observed else "unknown")
                denied = ("edit denied by user" in calls.get("call_1_edit", "") or
                          "Round 3 call 1 (edit config.txt): user denied this action" in summary)
                answer += f"\nEarliest edit: {'denied' if denied else 'unknown'}.\nEarliest version: {version}."
                if version != "unknown":
                    answer += f"\nOriginal reference: `{REFERENCE}`."
            item = {"type": "message", "id": f"msg_{task}", "role": "assistant",
                    "content": [{"type": "output_text", "text": answer}]}
        packet = {"type": "response.completed", "response": {"status": "completed", "output": [item]}}
        reply = f"data: {json.dumps(packet)}\n\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


def add_task(terminal, task):
    terminal.text = ""
    prompt = f"Mixed task {task:03d} inspect the fixture"
    if task in TARGETS:
        prompt += " and recall the earliest edit decision and source version"
    terminal.key(prompt, "> " + prompt)
    terminal.send(ESC + "OQ")
    reviews = []
    if task % 10 in (1, 2):
        reviews.append(("待批准 · 编辑", task % 10 == 2))
    if task % 10 == 2:
        reviews.append(("待批准 · 运行", True))
    for review, approve in reviews:
        terminal.expect(review, timeout=10)
        terminal.key("\t", "空回车拒绝")
        if approve:
            terminal.key("y", "> y")
        terminal.send("\r")
    terminal.expect(f"Task {task} done.", timeout=10)
    terminal.expect(COMPLETE, timeout=10)


def trace(path, task, operation):
    return [(int(size), round(int(ns) / 1e6, 6))
            for name, count, size, ns in (row.split("\t") for row in path.read_text().splitlines())
            if name == operation and int(count) == task]


def sample(app, args, env, database, metrics, task, request_sizes, repeats):
    saved = context(database)
    assert len(saved["tasks"]) == task and saved["finished"]
    first = saved["rounds"][0]["tools"][0]
    assert first["name"] == "read" and first["version"]
    assert saved["rounds"][2]["tools"][0]["result"] == "edit denied by user"
    answer = saved["rounds"][-1]["text"]
    assert answer.startswith(f"Task {task} done.")
    attempts = saved["model_attempts"][saved["tasks"][-1]["first_request"]:]
    projections = trace(metrics, task, "projection")
    assert [size for size, _ in projections] == request_sizes == [entry["uploaded_body_bytes"] for entry in attempts]
    assert all(size == entry["body_bytes"] for size, entry in zip(request_sizes, attempts))
    check_ms = [duration for _, duration in trace(metrics, task, "checkpoint")]
    line = subprocess.run([app, *args, "--resume"], capture_output=True, env=env, timeout=10, check=True)
    line = line.stdout.decode("utf-8")
    early_version = f"Earliest version: {first['version']}." in answer
    expected = f"[historical citation] {REFERENCE} task=1 round=1 version={first['version']}"
    assert (expected in line) == (task > 1)
    reopen_ms = []
    for _ in range(repeats):
        start = time.perf_counter_ns()
        with console(str(app), ["--tui", *args, "--resume", "--mono"], 30, 120, env) as terminal:
            terminal.expect(COMPLETE, timeout=10)
            terminal.key("ready probe", "> ready probe")
            reopen_ms.append(milliseconds(start))
    operations = Counter((entry["name"], entry["result"] == "edit denied by user")
                         for row in saved["rounds"] for entry in row["tools"])
    assert context(database) == saved
    return {"database_bytes": database.stat().st_size, "rounds": len(saved["rounds"]),
            "projection_bytes": request_sizes, "projection": measurements([ms for _, ms in projections]),
            "compacted_first_rounds": [entry["first_round"] for entry in attempts],
            "checkpoint_save": measurements(check_ms), "tui_reopen_to_draft_echo": measurements(reopen_ms),
            "earliest_denial_correct": "Earliest edit: denied." in answer,
            "earliest_version_correct": early_version and f"Original reference: `{REFERENCE}`." in answer,
            "archived_citation_preserved": expected in line if task > 1 else None,
            "reads": operations["read", False], "edits_approved": operations["edit", False],
            "edits_denied": operations["edit", True], "verifications": operations["run", False]}


def run(app, ctest, repeats, base, server):
    root = base / "workspace"
    root.mkdir()
    seed, config = root / SEED, root / CONFIG
    seed.write_text("Original version marker\n", encoding="utf-8")
    padding = "\n" + "\n".join(f"fixture line {i:03d} " + "x" * 67 for i in range(55)) + "\n"
    config.write_text("value=000" + padding, encoding="utf-8")
    (root / "CTestTestfile.cmake").write_text(
        f'add_test("empty config" [==[{sys.executable.replace(os.sep, "/")}]==] "-c" "print(42)")\n',
        encoding="utf-8")
    metrics = base / "trace.tsv"
    env = {**os.environ, "LAY_BENCHMARK_TRACE": str(metrics)}
    if os.name == "nt":
        env["LOCALAPPDATA"] = str(base / "state")
        state = base / "state/LayCode"
    else:
        env["XDG_STATE_HOME"] = str(base / "state")
        state = base / "state/lay-code"
    database = state / "sessions/session-mixed.sqlite3"
    profile = configure(base, server, "fixture.json")
    args = ["--workspace", str(root), "--session", "mixed", "--provider-config", str(profile),
            "--write", CONFIG, "--allow-run"]
    server.ctest, server.request_sizes = ctest.as_posix(), {}
    samples = {}
    first = 1
    for target in TARGETS:
        flags = ["--resume"] if first > 1 else []
        with console(str(app), ["--tui", *args, "--mono", *flags], 30, 120, env) as terminal:
            terminal.expect(COMPLETE if flags else READY, timeout=10)
            for task in range(first, target + 1):
                if task == 2:
                    seed.write_text("Externally rewritten version marker\n", encoding="utf-8")
                if task % 10 == 0:
                    config.write_text(f"value=external-{task:03d}" + padding, encoding="utf-8")
                add_task(terminal, task)
        samples[str(target)] = sample(app, args, env, database, metrics, target, server.request_sizes[target], repeats)
        print(f"Measured {target} tasks", file=sys.stderr, flush=True)
        first = target + 1

    saved = context(database)
    actions = [record for row in saved["rounds"] for record in row["tools"]]
    assert sum(entry["name"] == "read" for entry in actions) == 101
    assert sum(entry["edit"] is not None and entry["status"] == 1 for entry in actions) == 10
    assert sum(entry["result"] == "edit denied by user" and not entry["dispatched"] for entry in actions) == 10
    assert sum(entry.get("verification", {}).get("test_summary") == "CTest reported 1 passing test(s)."
               for entry in actions) == 10
    assert saved["requests"] == sum(len(sizes) for sizes in server.request_sizes.values())
    assert all(row["earliest_denial_correct"] for row in samples.values())
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("ctest", type=Path)
    parser.add_argument("--reopens", type=int, default=10)
    options = parser.parse_args()
    app, ctest = options.executable.resolve(strict=True), options.ctest.resolve(strict=True)
    if options.reopens < 1:
        parser.error("--reopens must be positive")
    with TemporaryDirectory(prefix="lay-p11-mixed-") as directory, ThreadingHTTPServer(("127.0.0.1", 0), Relay) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            samples = run(app, ctest, options.reopens, Path(directory), server)
        finally:
            server.shutdown()
    print(json.dumps({"executable_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
                      "terminal": "Windows PyWinPTY / ConPTY", "samples": samples}, indent=2))


if __name__ == "__main__":
    main()
