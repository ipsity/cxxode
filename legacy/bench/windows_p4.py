"""Record startup and process-capture samples from a staged Windows release."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading
import time

from startup import memory_reader, sample


def measure_process(driver, workspace, fixture, private_bytes, mode, cancel=0):
    with subprocess.Popen(
        [driver, workspace, sys.executable, "15000", str(cancel), fixture, mode],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ) as process:
        try:
            samples = []
            with ThreadPoolExecutor(max_workers=1) as executor:
                result = executor.submit(process.communicate, timeout=20)
                while not result.done():
                    try:
                        samples.append(private_bytes(process.pid))
                    except OSError:
                        if process.poll() is None:
                            raise
                    time.sleep(0.002)
                stdout, stderr = result.result()
            if process.returncode != 0 or stderr:
                raise RuntimeError(f"Process fixture failed: {process.returncode}, {stderr!r}")
            return json.loads(stdout), max(samples, default=0)
        finally:
            if process.poll() is None:
                process.kill()


def measure_buffered_reply(driver, workspace):
    gap_seconds = 0.12
    marker = "中文 SSE"

    def sse(kind, **fields):
        return f"data: {json.dumps({'type': kind, **fields}, ensure_ascii=False)}\n\n".encode()

    head = (
        sse("message_start", message={"role": "assistant", "content": [], "stop_reason": None})
        + sse("content_block_start", index=0, content_block={"type": "text", "text": ""})
        + sse("content_block_delta", index=0, delta={"type": "text_delta", "text": marker})
    )
    tail = (sse("content_block_stop", index=0) + sse("message_delta", delta={"stop_reason": "end_turn"})
            + sse("message_stop"))

    class Fixture(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            self.rfile.read(int(self.headers["Content-Length"]))
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(head) + len(tail)))
            self.end_headers()
            self.wfile.write(head)
            self.wfile.flush()
            self.server.head_ready = time.perf_counter_ns()
            time.sleep(gap_seconds)
            self.wfile.write(tail)

    observed = []
    with ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server:
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            for _ in range(20):
                result = subprocess.run([driver, "session", str(server.server_port), workspace, "0"],
                                        capture_output=True, timeout=10)
                received = time.perf_counter_ns()
                if result.returncode or marker.encode() not in result.stdout:
                    raise RuntimeError("The loopback reply was not shown")
                observed.append(round((received - server.head_ready) / 1_000_000, 3))
        finally:
            server.shutdown()
            thread.join()
    return {"server_gap_ms": gap_seconds * 1000, "p95_ms": sorted(observed)[18], "samples_ms": observed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("process_driver", type=Path)
    parser.add_argument("readonly_driver", type=Path)
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    if platform.system() != "Windows":
        parser.error("Windows private-byte measurements require Windows.")
    app = args.executable.resolve(strict=True)
    process_driver = args.process_driver.resolve(strict=True)
    readonly_driver = args.readonly_driver.resolve(strict=True)
    fixture = Path(__file__).resolve().parents[1] / "tests/fixtures/process_child.py"
    windows = Path(os.environ["SystemRoot"])
    os.environ["PATH"] = os.pathsep.join(map(str, (windows / "System32", windows)))
    private_bytes = memory_reader()
    with ThreadPoolExecutor(max_workers=1) as executor:
        executor.submit(lambda: None).result()
        first = sample(app, executor, private_bytes)
        warm = [sample(app, executor, private_bytes) for _ in range(100)]
    with TemporaryDirectory(prefix="lay-code-benchmark-") as temporary:
        workspace = Path(temporary)
        driver = shutil.copyfile(process_driver, workspace / process_driver.name)
        reply_driver = shutil.copyfile(readonly_driver, workspace / readonly_driver.name)
        for dependency in app.parent.glob("*.dll"):
            shutil.copyfile(dependency, workspace / dependency.name)
        flood, flood_memory = measure_process(driver, workspace, fixture, private_bytes, "flood")
        if flood["status"] != "command exited 0":
            raise RuntimeError(f"Flood fixture did not complete: {flood['status']}")
        cancellation = []
        for _ in range(5):
            result, _ = measure_process(driver, workspace, fixture, private_bytes, "hold", cancel=180)
            if result["status"] != "command cancelled":
                raise RuntimeError(f"Cancellation fixture did not complete: {result['status']}")
            cancellation.append(result["elapsed_ms"] - 180)
        buffered_reply = measure_buffered_reply(reply_driver, workspace)
    startup, idle = zip(*warm)
    print(json.dumps({
        "os": platform.platform(), "compiler": args.compiler,
        "build": "Release x64 with Schannel, stdout pipe, Windows-only DLL search path",
        "cache": "first observed sample, OS caches not reset",
        "first_ms": first[0], "first_private_bytes": first[1], "warm_p50_ms": statistics.median(startup),
        "warm_p95_ms": sorted(startup)[94], "idle_max_private_bytes": max(idle),
        "startup_ms": startup, "idle_private_bytes": idle,
        "executable_bytes": app.stat().st_size,
        "executable_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
        "accompanying_dll_bytes": sum(path.stat().st_size for path in app.parent.glob("*.dll")),
        "capture": {
            "duration_ms": flood["elapsed_ms"], "sampled_driver_peak_private_bytes": flood_memory,
            "sampling_interval_ms_requested": 2, "child_private_bytes": None,
            "stdout": {name: flood["stdout"][name] for name in ("total", "omitted")},
            "stderr": {name: flood["stderr"][name] for name in ("total", "omitted")},
        },
        "cancellation_driver_cleanup_ms": cancellation,
        "buffered_reply": buffered_reply,
    }, indent=2))


if __name__ == "__main__":
    main()
