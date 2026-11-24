"""Measure the CLI's first text preview against a delayed loopback Responses stream."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import json
from pathlib import Path
import platform
import subprocess
from tempfile import TemporaryDirectory
import threading
import time


GAP_SECONDS = 0.12
MARKER = "中文预览"


def sse(type_, **fields):
    return ("data: " + json.dumps({"type": type_, **fields}, ensure_ascii=False) + "\n\n").encode()


class Fixture(BaseHTTPRequestHandler):
    head = sse("response.output_text.delta", delta=MARKER)
    tail = sse("response.completed", response={"status": "completed", "output": [
        {"type": "message", "role": "assistant", "content": [
            {"type": "output_text", "text": MARKER + "，完整回复"},
        ]},
    ]})

    def log_message(self, *_):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(self.head) + len(self.tail)))
        self.end_headers()
        self.server.sent_ns = time.perf_counter_ns()
        self.wfile.write(self.head)
        self.wfile.flush()
        time.sleep(GAP_SECONDS)
        self.wfile.write(self.tail)


def read_preview(pipe):
    preview = pipe.readline()
    return time.perf_counter_ns(), preview


def measure(app):
    samples = []
    with TemporaryDirectory(prefix="lay-code-preview-") as storage, \
            ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server, \
            ThreadPoolExecutor(max_workers=1) as executor:
        root = Path(storage)
        config = root / "providers.json"
        config.write_text(json.dumps({"default_provider": "fixture", "providers": {"fixture": {
            "protocol": "responses", "base_url": f"http://127.0.0.1:{server.server_port}",
            "model": "fixture", "api_key_env": None,
        }}}), encoding="utf-8")
        command = [app, "--workspace", root, "--provider-config", config, "--question", "Preview"]
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for _ in range(20):
                with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                    try:
                        observed, preview = executor.submit(read_preview, process.stderr).result(timeout=5)
                        output, _ = process.communicate(timeout=5)
                    finally:
                        if process.poll() is None:
                            process.kill()
                if (process.returncode or preview.strip() != ("[preview] " + MARKER).encode() or
                        (MARKER + "，完整回复").encode() not in output or b"dispatches=0" not in output):
                    raise RuntimeError(f"Unexpected CLI result: {process.returncode}, {preview!r}, {output!r}")
                samples.append(round((observed - server.sent_ns) / 1_000_000, 3))
        finally:
            server.shutdown()
            thread.join()
    return {"os": platform.platform(), "executable_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
            "protocol": "responses", "terminal": "stderr pipe", "server_gap_ms": GAP_SECONDS * 1000,
            "p95_ms": sorted(samples)[18], "samples_ms": samples}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    arguments = parser.parse_args()
    print(json.dumps(measure(arguments.executable.resolve(strict=True)), indent=2))
