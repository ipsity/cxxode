"""Pair serial and two-worker runs of the isolated P22 host prototype."""

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading
import time
from zipfile import ZipFile

from live_quality import context
from startup import memory_reader


SOURCE = "f9c8abf"
REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tests/contract"))
from providers import completed, message


WARMUP = 3
SAMPLES = 30
MODEL_DELAY_MS = 30
SPEED_GATE = 10
MEMORY_RATIO_GATE = 1.25
WORKLOADS = {
    "compare-session-interfaces": ("Compare the session, store, edit and provider interfaces.", [
        ("read", {"path": path, "line_count": 35}) for path in
        ("src/core/session.hpp", "src/store/session_store.hpp", "src/tools/edit.hpp", "src/model/provider.hpp")]),
    "locate-cancellation-across-modules": ("Locate the cancellation API in both tools and model adapters.", [
        ("search", {"path": path, "query": "StopRequested", "max_results": 12}) for path in
        ("src/tools", "src/model")]),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def distribution(values):
    return {"count": len(values), "p50": round(statistics.median(values), 6),
            "p95": round(sorted(values)[math.ceil(0.95 * len(values)) - 1], 6)}


class Relay(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        replies = self.server.replay
        index = len(self.server.requests)
        reply = replies[index]
        self.server.requests.append(body)
        started = time.perf_counter_ns()
        time.sleep(MODEL_DELAY_MS / 1000)
        self.server.wait_time.append((time.perf_counter_ns() - started) / 1e6)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


def sample(app, directory, workspace, config, question, server, width, mode, sample_id, memory):
    session = f"{sample_id}-{mode}"
    trace = directory / (session + ".tsv")
    env = {**os.environ, "LAY_BENCHMARK_TRACE": str(trace), "LAY_P22_READ_WORKERS": str(mode)}
    if os.name == "nt":
        env["LOCALAPPDATA"] = str(directory)
        state = directory / "LayCode"
    else:
        env["XDG_STATE_HOME"] = str(directory)
        state = directory / "lay-code"
    server.requests = []
    server.wait_time = []
    observed_memory = []
    ended = threading.Event()
    started = time.perf_counter_ns()
    with subprocess.Popen([app, "--workspace", workspace, "--provider-config", config, "--session", session,
                           "--question", question], env=env, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
        def measure():
            while not ended.is_set():
                try:
                    observed_memory.append(memory(process.pid))
                except (OSError, ProcessLookupError):
                    break
                ended.wait(0.004)

        sampler = threading.Thread(target=measure)
        sampler.start()
        try:
            stdout, stderr = process.communicate(timeout=10)
            elapsed = (time.perf_counter_ns() - started) / 1e6
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        finally:
            ended.set()
            sampler.join()
        assert process.returncode == 0 and b"Recorded observations." in stdout, (stdout, stderr)

    saved = context(state / "sessions" / f"session-{session}.sqlite3")
    records = saved["rounds"][0]["tools"]
    ids = [f"probe-{i}" for i in range(width)]
    assert [row["id"] for row in records] == ids and all(row["dispatched"] and row["status"] == 1 for row in records)
    assert saved["requests"] == len(server.requests) == 2 and saved["dispatches"] == width and saved["finished"]
    inputs = [json.loads(raw) for raw in server.requests]
    assert [row["call_id"] for row in inputs[1]["input"] if row.get("type") == "function_call_output"] == ids
    identity = [sha(re.sub(rb"UTC [0-9.T:Z-]+", b"UTC observation-time", raw)) for raw in server.requests]
    metrics = {}
    for line in trace.read_text(encoding="ascii").splitlines():
        operation, _, size, ns = line.split("\t")
        metrics.setdefault(operation, []).append((int(size), int(ns) / 1e6))
    assert len(metrics["tool_local"]) == len(metrics["tool_content_io"]) == width
    assert len(metrics["projection"]) == 2 and len(metrics["tool_batch"]) == 1
    assert metrics["tool_batch"][0][0] == width
    assert len(metrics["checkpoint"]) == 2 * width + 6
    return {
        "end_to_end_ms": elapsed,
        "workspace_tool_sum_ms": sum(ms for _, ms in metrics["tool_local"]),
        "file_content_io_sum_ms": sum(ms for _, ms in metrics["tool_content_io"]),
        "tool_batch_with_commits_ms": metrics["tool_batch"][0][1],
        "sqlite_commits_sum_ms": sum(ms for _, ms in metrics["checkpoint"]),
        "projection_sum_ms": sum(ms for _, ms in metrics["projection"]),
        "client_model_first_byte_sum_ms": sum(item["first_byte_us"] / 1000 for item in saved["model_attempts"]),
        "relay_delay_sum_ms": sum(server.wait_time),
        "peak_sampled_private_bytes": max(observed_memory),
        "checkpoint_count": len(metrics["checkpoint"]), "request_hashes_without_times": identity,
    }


def measure_workload(name, index, question, calls, app, root, workspace, config, server, memory):
    batch = completed([{"type": "function_call", "id": f"fc_probe-{i}", "call_id": f"probe-{i}",
                        "name": tool, "arguments": json.dumps(args)} for i, (tool, args) in enumerate(calls)])
    server.replay = [batch, completed([message("Recorded observations.")])]
    observations = {1: [], 2: []}
    request_hashes = None
    for i in range(WARMUP + SAMPLES):
        modes = (1, 2) if (i - WARMUP) % 2 == 0 else (2, 1)
        for mode in modes:
            values = sample(app, root, workspace, config, question, server, len(calls), mode, f"{index}-{i}", memory)
            hashes = values.pop("request_hashes_without_times")
            if request_hashes is None:
                request_hashes = hashes
            assert request_hashes == hashes, (name, i, mode, request_hashes, hashes)
            if i >= WARMUP:
                observations[mode].append(values)
    summary = {str(mode): {metric: distribution([row[metric] for row in rows]) for metric in rows[0]}
               for mode, rows in observations.items()}
    serial, paired = (summary[str(mode)]["end_to_end_ms"]["p95"] for mode in (1, 2))
    peak = {mode: max(row["peak_sampled_private_bytes"] for row in rows) for mode, rows in observations.items()}
    speed_gain = 100 * (1 - paired / serial)
    return {"question": question, "batch": calls, "responses_sha256": [sha(item) for item in server.replay],
            "request_hashes_without_times": request_hashes, "statistics": summary,
            "peak_private_bytes": {str(mode): value for mode, value in peak.items()},
            "end_to_end_p95_gain_percent": round(speed_gain, 3),
            "paired_end_to_end_savings_ms": distribution([a["end_to_end_ms"] - b["end_to_end_ms"]
                                                          for a, b in zip(observations[1], observations[2])]),
            "speed_pass": speed_gain >= SPEED_GATE, "memory_pass": peak[2] <= MEMORY_RATIO_GATE * peak[1],
            "samples": observations}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--raw", type=Path, required=True)
    args = parser.parse_args()
    app = args.executable.resolve(strict=True)
    source = subprocess.check_output(["git", "rev-parse", SOURCE], cwd=REPO, text=True).strip()
    archive = subprocess.check_output(["git", "archive", "--format=zip", source, "src"], cwd=REPO)
    memory = memory_reader()
    cases = {}
    with TemporaryDirectory(prefix="lay-p22-") as temp, ThreadingHTTPServer(("127.0.0.1", 0), Relay) as server:
        directory = Path(temp)
        workspace = directory / "root"
        with ZipFile(BytesIO(archive)) as files:
            files.extractall(workspace)
        config = directory / "provider.json"
        config.write_text(json.dumps({"default_provider": "fixture", "providers": {"fixture": {
            "protocol": "responses", "base_url": f"http://127.0.0.1:{server.server_port}/v1",
            "model": "p22-recording", "api_key_env": None,
        }}}), encoding="utf-8")
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            for index, (name, (question, batch)) in enumerate(WORKLOADS.items()):
                cases[name] = measure_workload(name, index, question, batch, app, directory, workspace, config,
                                              server, memory)
        finally:
            server.shutdown()
            worker.join()
    args.raw.write_text(json.dumps({name: case.pop("samples") for name, case in cases.items()}, indent=2) + "\n",
                        encoding="utf-8")
    eligible = all(case["speed_pass"] and case["memory_pass"] for case in cases.values())
    print(json.dumps({"milestone": "P22", "source": source, "workspaces_sha256": sha(archive),
                      "experiment_executable_sha256": sha(app.read_bytes()), "protocol": "responses",
                      "model": "recorded local Responses packets", "delay_per_response_ms": MODEL_DELAY_MS,
                      "warmup_pairs_per_workload": WARMUP, "measured_pairs_per_workload": SAMPLES,
                      "order": "15 serial-first and 15 prototype-first pairs, alternating", "modes": [1, 2],
                      "p95_gain_threshold_percent_for_each_workload": SPEED_GATE,
                      "peak_memory_ratio_limit": MEMORY_RATIO_GATE, "performance_gate_pass": eligible,
                      "production_decision": "needs reliability tests" if eligible else "retain serial",
                      "raw_samples_sha256": sha(args.raw.read_bytes()), "workloads": cases}, indent=2))


if __name__ == "__main__":
    main()
