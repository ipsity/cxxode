"""Measure P17 request checkpoints against a repeated loopback 429 + 200."""

import argparse
from contextlib import closing
import hashlib
from http.server import ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "contract"))
from providers import Handler, completed, message, stream


def distribution(values):
    ordered = sorted(values)
    return {"samples": len(values), "p50_ms": round(ordered[math.ceil(len(values) * .50) - 1] / 1e6, 3),
            "p95_ms": round(ordered[math.ceil(len(values) * .95) - 1] / 1e6, 3)}


def measure(executable, tasks, directory, server):
    trace = directory / "trace.tsv"
    config = directory / "providers.json"
    root = directory / "workspace"
    root.mkdir()
    state = directory / "state"
    config.write_text(json.dumps({"default_provider": "relay", "providers": {"relay": {
        "model": "fixture", "protocol": "responses", "api_key_env": "LAY_BENCH_RETRY_KEY",
        "base_url": f"http://127.0.0.1:{server.server_port}/v1",
    }}}), encoding="utf-8")
    environment = {**os.environ, "LAY_BENCH_RETRY_KEY": "fixture-secret", "LAY_BENCHMARK_TRACE": str(trace),
                   "LOCALAPPDATA": str(state), "XDG_STATE_HOME": str(state)}
    command = [executable, "--provider-config", config, "--workspace", root, "--session", "measure-retry"]
    for task in range(tasks):
        server.steps = [("POST", "/v1/responses", lambda _: (b"", 429, "text/plain")),
                        ("POST", "/v1/responses", stream(completed([message("OK")])))]
        server.errors = []
        result = subprocess.run([*command, "--question" if task == 0 else "--follow-up", f"Task {task + 1}"],
                                env=environment, capture_output=True, timeout=10)
        assert result.returncode == 0 and not server.steps and not server.errors, (result.stderr, server.errors)
    db, = state.rglob("session-measure-retry.sqlite3")
    with closing(sqlite3.connect(db)) as connection:
        snapshot = json.loads(connection.execute("SELECT snapshot FROM session").fetchone()[0])
        phases = [row[0] for row in connection.execute("SELECT phase FROM checkpoints ORDER BY revision")]
    if not trace.is_file():
        raise SystemExit("Build the executable with LAY_BENCHMARK_METRICS=ON.")
    numbers = [int(line.split("\t")[3]) for line in trace.read_text().splitlines()
               if line.startswith("checkpoint\t")]
    assert phases[0] == "input" and len(phases) - 1 == len(numbers)
    timings = {phase: [elapsed for observed, elapsed in zip(phases[1:], numbers) if observed == phase]
               for phase in ("request", "request_result")}
    assert all(len(values) == tasks * 2 for values in timings.values())
    assert snapshot["requests"] == tasks * 2 and snapshot["dispatches"] == 0
    return {"executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
            "tasks": tasks, "server_posts": tasks * 2,
            "unobserved_usage": sum(attempt["input_tokens"] is None for attempt in snapshot["model_attempts"]),
            "request_reservation": distribution(timings["request"]),
            "request_receipt": distribution(timings["request_result"]),
            "two_checkpoints_per_post": distribution([a + b for a, b in zip(*timings.values())])}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--tasks", type=int, default=30, choices=range(1, 101), metavar="1-100")
    args = parser.parse_args()
    with TemporaryDirectory(prefix="lay-retry-bench-") as directory:
        with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                report = measure(args.executable.resolve(), args.tasks, Path(directory), server)
                print(json.dumps(report, indent=2))
            finally:
                server.shutdown()
