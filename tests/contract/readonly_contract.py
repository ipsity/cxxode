from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading
import time


SOURCE = "src/带 空格.txt"
QUERY = "空配置"


def event(kind, **fields):
    document = json.dumps({"type": kind, **fields}, ensure_ascii=False, indent=2)
    return (f"event: {kind}\r\n" +
            "".join(f"data: {line}\r\n" for line in document.splitlines()) + "\r\n").encode()


START = event("message_start", message={"role": "assistant", "content": [], "stop_reason": None})


def tail(reason):
    return event("message_delta", delta={"stop_reason": reason}) + event("message_stop")


def text(index, value):
    return (event("content_block_start", index=index, content_block={"type": "text", "text": ""}) +
            event("content_block_delta", index=index, delta={"type": "text_delta", "text": value}) +
            event("content_block_stop", index=index))


def tool_start(index, identifier, name):
    return event("content_block_start", index=index,
                 content_block={"type": "tool_use", "name": name, "id": identifier, "input": {}})


def part(index, value):
    return event("content_block_delta", index=index, delta={"type": "input_json_delta", "partial_json": value})


def tool(index, identifier, name, arguments):
    encoded = json.dumps(arguments, ensure_ascii=False)
    midpoint = len(encoded) // 2
    return (tool_start(index, identifier, name) + part(index, encoded[:midpoint]) +
            part(index, encoded[midpoint:]) + event("content_block_stop", index=index))


def answer(value):
    return START + text(0, value) + tail("end_turn")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            raw = self.rfile.read(int(self.headers["Content-Length"]))
            assert self.path == "/v1/messages"
            assert self.headers["anthropic-version"] == "2023-06-01"
            assert "x-api-key" not in self.headers
            payload = json.loads(raw)
            assert payload["model"] == "fixture-model"
            assert [tool["name"] for tool in payload["tools"]] == ["read", "search"]
            request = self.server.steps.popleft()
            self.server.timestamps.append(time.monotonic())
            self.server.requests.append(raw)
            request(self, json.loads(raw))
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
        except Exception as error:
            self.server.errors.append(repr(error))
            self.respond(b"", 400)

    def respond(self, body, code=200, retry_after=None, split_at=None):
        self.send_response(code)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("request-id", "req_fixture")
        if retry_after is not None:
            self.send_header("Retry-After", str(retry_after))
        self.end_headers()
        if split_at is not None:
            self.wfile.write(body[:split_at])
            self.wfile.flush()
            time.sleep(0.8)
            self.server.late_delivery.set()
            body = body[split_at:]
        self.wfile.write(body)


def run(arguments, expected_exit, *, data=None, env=None):
    process = subprocess.run(arguments, input=data, capture_output=True, timeout=10, env=env)
    stdout = process.stdout.decode("utf-8").replace("\r\n", "\n")
    stderr = process.stderr.decode("utf-8").replace("\r\n", "\n")
    assert process.returncode == expected_exit, (arguments, process.returncode, stdout, stderr)
    return stdout, stderr


def create_workspace(base):
    workspace = base / "只读 工作区"
    for path, content in {
        SOURCE: f"header\r\n{QUERY}=允许\r\ntail\r\n".encode(),
        ".env": b"PRIVATE_SENTINEL",
        ".git/config": b"PRIVATE_SENTINEL",
        "private/key.txt": b"PRIVATE_SENTINEL",
        "binary.txt": b"\x00\xff",
        "long.txt": (b"x" * 40000) + b"\ntail\n",
    }.items():
        target = workspace / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    outside = base / "outside"
    outside.mkdir()
    (outside / "secret.txt").write_bytes(b"PRIVATE_SENTINEL")
    os.link(outside / "secret.txt", workspace / "hardlink.txt")
    if os.name == "nt":
        subprocess.run(["cmd", "/c", "mklink", "/J", str(workspace / "redirect"), str(outside)],
                       check=True, capture_output=True)
    else:
        (workspace / "redirect").symlink_to(outside, target_is_directory=True)
    return workspace


def scenarios(program, cli, server, workspace):
    read = tool(0, "read1", "read", {"path": SOURCE})
    lookup = tool(0, "search1", "search", {"path": ".", "query": QUERY, "file_suffix": ".txt"})
    stop = tail("tool_use")
    decoded, _ = run([program, "codec"], 0,
                     data=b"\xef\xbb\xbf" + START + b": keepalive\n\n" +
                     text(0, "中文") + tool(1, "read1", "read", {"path": SOURCE}) + stop)
    assert decoded == f"中文\nread1 read {SOURCE}\n", decoded
    decoded, _ = run([program, "codec"], 0, data=START +
                     tool(0, "run1", "run", {"program": "ctest", "args": ["", "中文 空格"], "path": "."}) + stop)
    assert decoded == "run1 run .\n[]\n[中文 空格]\n", decoded

    def session(steps, expected, code=0, cancel_ms=0, mode="session"):
        server.steps = deque(steps)
        server.errors = []
        server.requests = []
        server.timestamps = []
        stdout, _ = run([program, mode, str(server.server_port), workspace, str(cancel_ms)], code)
        assert not server.steps and not server.errors, (server.steps, server.errors)
        assert expected in stdout, stdout
        assert "PRIVATE_SENTINEL" not in stdout, stdout
        return stdout

    def send(content):
        return lambda handler, request: handler.respond(content)

    def with_results(check, final="Recorded the tool results."):
        def exchange(handler, request):
            assert [item["role"] for item in request["messages"]] == ["user", "assistant", "user"], request
            check(request["messages"][1]["content"], request["messages"][2]["content"])
            handler.respond(answer(final))
        return exchange

    def present_search(blocks, results):
        assert blocks[0]["name"] == "search", blocks
        assert results[0]["tool_use_id"] == "search1", results
        assert results[0]["is_error"] is False, results
        assert f"{SOURCE}:2: {QUERY}=允许" in results[0]["content"], results

    stdout = session([lambda handler, request: handler.respond(b"", 429, retry_after=1),
                      send(START + lookup + stop),
                      with_results(present_search, f"Located at {SOURCE}:2")],
                     "end_turn requests=3 dispatches=1")
    assert f"Located at {SOURCE}:2" in stdout
    assert server.requests[0] == server.requests[1] and server.timestamps[1] - server.timestamps[0] >= 0.95

    first = START + text(0, "before") + tool(1, "list", "read", {"path": "."}) + text(2, "between")
    first += tool(3, "lines", "read", {"path": SOURCE, "start_line": 2, "line_count": 1}) + text(4, "after")

    def ordered(blocks, results):
        assert [block.get("text", block.get("id")) for block in blocks] == [
            "before", "list", "between", "lines", "after"]
        assert [item["tool_use_id"] for item in results] == ["list", "lines"]
        assert "src/" in results[0]["content"]
        assert all(name not in results[0]["content"] for name in (".env", ".git", "private", "redirect"))
        assert results[1]["content"] == f"{SOURCE}\n2: {QUERY}=允许\nnext_line=3\n", results

    session([send(first + stop), with_results(ordered)], "end_turn requests=2 dispatches=2")

    def batch_error(blocks, results):
        assert [item["content"] for item in results] == ["batch_argument_error", "invalid_integer_argument"]
        assert all(item["is_error"] for item in results)

    for bad_line in (0, True, 10**19):
        rejected = tool(1, "invalid", "read", {"path": SOURCE, "start_line": bad_line})
        session([send(START + read + rejected + stop), with_results(batch_error)],
                "end_turn requests=2 dispatches=0")

    for target, dispatches in (("../outside/secret.txt", 0), (".env", 0), (".git/config", 0),
                               ("private/key.txt", 0), ("redirect/secret.txt", 1), ("hardlink.txt", 1),
                               ("src\\带 空格.txt", 0), ("src/带 空格.txt:secret", 0), ("binary.txt", 1)):
        def denied(blocks, results):
            assert results[0]["is_error"] is True, (target, results)
        request = tool(0, "denied", "read", {"path": target})
        session([send(START + request + stop), with_results(denied)], f"end_turn requests=2 dispatches={dispatches}")

    def long_line(blocks, results):
        assert "line exceeds the 32768-byte tool result limit" in results[0]["content"]
        assert "next_line=2" in results[0]["content"]

    session([send(START + tool(0, "long", "read", {"path": "long.txt"}) + stop), with_results(long_line)],
            "end_turn requests=2 dispatches=1")

    cutoff = tool_start(1, "partial", "read") + part(1, '{"path": "s')
    edit = tool(0, "edit1", "edit", {"path": SOURCE, "old": "header", "new": "changed"})
    duplicate = tool(1, "read1", "read", {"path": SOURCE})
    duplicate_keys = (tool_start(0, "read1", "read") + part(0, '{"path":".","path":"src"}') +
                      event("content_block_stop", index=0))
    failures = [
        (START + read + cutoff, "protocol_error"),
        (START + edit + cutoff, "protocol_error"),
        (START + read + duplicate + stop, "protocol_error"),
        (START + duplicate_keys + stop, "protocol_error"),
        (START + read + event("error", error={"type": "overloaded_error"}), "transport_error"),
        (START + read + tail("max_tokens"), "response_limit"),
        (START + read + event("message_delta", delta={"stop_reason": "tool_use"}), "protocol_error"),
        (b"data: " + b"x" * 1048576 + b"\n\n", "response_limit"),
    ]
    original = (workspace / SOURCE).read_bytes()
    for broken, reason in failures:
        session([send(broken)], f"{reason} requests=1 dispatches=0", 1)
    assert (workspace / SOURCE).read_bytes() == original

    server.late_delivery = threading.Event()
    session([lambda handler, request: handler.respond(START + read + stop, split_at=len(START + read))],
            "cancelled requests=1 dispatches=0", 1, cancel_ms=300)
    assert server.late_delivery.wait(2), "the server never attempted the late reply"
    session([lambda handler, request: handler.respond(b"", 401)],
            "http_error requests=1 dispatches=0 HTTP 401 (request-id req_fixture)", 1)

    rejected = json.dumps({"type": "error", "error": {
        "type": "invalid_request_error", "message": "Prompt is too long for this model."
    }}).encode()

    def repair_context(handler, request):
        messages = request["messages"]
        assert messages[0]["content"][0]["text"] == "Find the source location", messages[0]
        assert "Host facts" in messages[0]["content"][1]["text"]
        assert 3 <= len(messages) < 13
        for proposed, observed in zip(messages[1::2], messages[2::2]):
            assert proposed["content"][0]["id"] == observed["content"][0]["tool_use_id"]
        assert messages[-1]["content"][0]["tool_use_id"] == "past-5"
        handler.respond(event("message_start", message={
            "role": "assistant", "content": [], "stop_reason": None,
            "usage": {"input_tokens": 23, "cache_read_input_tokens": 11, "output_tokens": 1},
        }) + text(0, "context repaired") + event("message_delta", delta={"stop_reason": "end_turn"},
                                                  usage={"output_tokens": 7}) + event("message_stop"))

    output = session([lambda handler, request: handler.respond(rejected, 400), repair_context],
                     "end_turn requests=2 dispatches=0", mode="context")
    assert "projection 0 6 0 0" in output and " 34 7" in output
    assert len(server.requests[1]) < len(server.requests[0])
    session([lambda handler, request: handler.respond(rejected, 400)] * 2,
            "context_limit requests=2 dispatches=0", 1, mode="context")

    environment = os.environ.copy()
    environment.pop("ANTHROPIC_API_KEY", None)
    _, stderr = run([cli, "--workspace", workspace, "--model", "fixture-model"], 2, env=environment)
    assert "Set ANTHROPIC_API_KEY" in stderr, stderr


def main():
    program, cli = sys.argv[1:]
    with TemporaryDirectory(prefix="lay-code-") as temporary:
        workspace = create_workspace(Path(temporary))
        with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                scenarios(program, cli, server, workspace)
            finally:
                server.shutdown()
                thread.join()
    print("Verified workspace, byte-split SSE, tool round trips, retries, and cancellation.")


if __name__ == "__main__":
    main()
