from contextlib import closing
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


SOURCE = "文件.txt"
KEY_VARIABLE = "LAY_CODE_FIXTURE_PROVIDER_TOKEN"


def sse(value):
    if isinstance(value, str):
        return f"data: {value}\r\n\r\n".encode()
    return ("data: " + json.dumps(value, ensure_ascii=False) + "\r\n\r\n").encode()


def chat(delta, finish=None):
    return sse({"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]})


def call(index, identifier, name, arguments):
    return {"index": index, "id": identifier, "type": "function",
            "function": {"name": name, "arguments": arguments}}


def response(kind, **fields):
    return sse({"type": kind, **fields})


def completed(output, identifier="resp_fixture"):
    return response("response.completed", response={
        "id": identifier, "status": "completed", "output": output,
        "usage": {"input_tokens": 21, "output_tokens": 4},
    })


def message(text):
    return {"id": "msg_answer", "type": "message", "role": "assistant",
            "content": [{"type": "output_text", "text": text, "annotations": []}]}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def handle_request(self, method, payload):
        try:
            expected_method, path, action = self.server.steps.pop(0)
            assert method == expected_method and self.path == path, (method, self.path)
            assert self.headers.get("Authorization") == "Bearer fixture-secret"
            assert "x-api-key" not in self.headers and "anthropic-version" not in self.headers
            body, status, content_type = action(payload)
        except Exception as error:
            self.server.errors.append(repr(error))
            body, status, content_type = b"", 400, "text/plain"
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        if status == 307:
            self.send_header("Location", "/redirect-target")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            pass

    def do_GET(self):
        self.handle_request("GET", None)

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        self.handle_request("POST", json.loads(body))


def stream(body):
    return lambda _: (body, 200, "text/event-stream")


def tests(executable, base, server):
    root = base / "工作区"
    root.mkdir()
    (root / SOURCE).write_text("gateway evidence", encoding="utf-8")
    host = f"http://127.0.0.1:{server.server_port}"
    config = base / "providers.json"
    profiles = {"default_provider": "relay", "providers": {
        "relay": {"protocol": "responses", "base_url": host + "/relay/v1/responses",
                  "model": "relay/model:1", "api_key_env": KEY_VARIABLE},
        "chat": {"protocol": "chat-completions", "base_url": host, "api_key_env": KEY_VARIABLE},
    }}
    config.write_text(json.dumps(profiles), encoding="utf-8")
    state = base / "state"
    env = {**os.environ, KEY_VARIABLE: "fixture-secret", "LOCALAPPDATA": str(state), "XDG_STATE_HOME": str(state)}

    def cli(arguments, steps, code=0, environment=env):
        server.steps = list(steps)
        server.errors = []
        outcome = subprocess.run([executable, "--provider-config", config, *arguments], env=environment,
                                 input=b"", capture_output=True, timeout=10)
        stdout = outcome.stdout.decode("utf-8").replace("\r\n", "\n")
        stderr = outcome.stderr.decode("utf-8").replace("\r\n", "\n")
        assert not server.steps and not server.errors, (server.steps, server.errors, stdout, stderr)
        assert outcome.returncode == code, (outcome.returncode, stdout, stderr)
        assert "fixture-secret" not in stdout + stderr
        return stdout, stderr

    def catalog(_):
        return json.dumps({"data": [{"id": "relay/model:1"}, {"id": "aaa"}, {"id": "aaa"}]}).encode(), 200, "application/json"

    listed, _ = cli(["--list-models"], [("GET", "/relay/v1/models", catalog)])
    assert listed == "aaa\nrelay/model:1\n", listed
    _, error = cli(["--list-models"], [], 2, {name: value for name, value in env.items() if name != KEY_VARIABLE})
    assert KEY_VARIABLE in error
    _, error = cli(["--list-models"], [("GET", "/relay/v1/models", lambda _: (b"", 307, "text/plain"))], 2)
    assert "HTTP 307" in error

    starting = ["--workspace", root, "--question", "Find the file"]
    serialized = json.dumps({"path": SOURCE}, ensure_ascii=False)
    fragmented = (
        chat({"tool_calls": [call(0, "call_file", "read", serialized[:11])]}) +
        chat({"tool_calls": [{"index": 0, "function": {"arguments": serialized[11:]}}]}) +
        chat({}, "tool_calls")
    )
    chat_args = [*starting, "--provider", "chat", "--model", "custom/chat"]

    def initial_chat(payload):
        assert payload["model"] == "custom/chat" and payload["stream"] is True
        assert payload["messages"][0]["role"] == "system"
        assert payload["tools"][0]["function"]["name"] == "read"
        return fragmented + sse("[DONE]"), 200, "text/event-stream"

    def final_chat(payload):
        assistant, observed = payload["messages"][-2:]
        call_item = assistant["tool_calls"][0]
        assert call_item["id"] == observed["tool_call_id"] == "call_file"
        assert json.loads(call_item["function"]["arguments"]) == {"path": SOURCE}
        assert "gateway evidence" in observed["content"]
        return chat({"content": "Chat 完成"}) + chat({}, "stop") + sse("[DONE]"), 200, "text/event-stream"

    stdout, _ = cli(chat_args, [("POST", "/v1/chat/completions", initial_chat),
                              ("POST", "/v1/chat/completions", final_chat)])
    assert "Chat 完成" in stdout and "dispatches=1" in stdout
    broken, _ = cli(chat_args, [("POST", "/v1/chat/completions", stream(fragmented))], 1)
    assert "protocol_error" in broken and "dispatches=0" in broken

    native = {"id": "rs_fixture", "type": "reasoning", "summary": [], "encrypted_content": "opaque-fixture"}
    item = {"id": "fc_a", "type": "function_call", "call_id": "call_file",
            "name": "read", "arguments": serialized}

    def initial_response(payload):
        assert payload["model"] == "relay/model:1" and payload["store"] is False
        assert payload["tools"][0]["name"] == "read" and payload["input"][0]["role"] == "user"
        body = response("response.created", response={"id": "resp_fixture"})
        body += response("response.output_item.added", output_index=0, item=native)
        body += response("response.output_item.done", output_index=0, item=native)
        body += response("response.output_item.added", output_index=1, item={**item, "arguments": ""})
        body += response("response.function_call_arguments.delta", output_index=1, delta=serialized)
        body += response("response.output_item.done", output_index=1, item=item)
        body += response("response.metadata")
        return body + completed([native, item], "resp_gateway"), 200, "text/event-stream"

    def final_response(payload, answer):
        state, proposed, observed = payload["input"][-3:]
        assert state == native and proposed["type"] == "function_call"
        assert proposed["call_id"] == observed["call_id"] == "call_file"
        assert observed["type"] == "function_call_output" and "gateway evidence" in observed["output"]
        return completed([message(answer)]), 200, "text/event-stream"

    split = f"Responses 完成\n- 文件：`{SOURCE}`\n- 行号：第 1 行"
    stdout, _ = cli([*starting, "--session", "provider-contract"], [
        ("POST", "/relay/v1/responses", initial_response),
        ("POST", "/relay/v1/responses", lambda payload: final_response(payload, split)),
    ])
    citation = f"[citation] {SOURCE}:1\n"
    assert "Responses 完成" in stdout and "dispatches=1" in stdout and citation in stdout
    database_dir = state / ("LayCode" if os.name == "nt" else "lay-code") / "sessions"
    session_args = ["--workspace", root, "--session", "provider-contract"]
    database_path = database_dir / "session-provider-contract.sqlite3"

    def saved():
        with closing(sqlite3.connect(database_path)) as database:
            return json.loads(database.execute("SELECT snapshot FROM session").fetchone()[0])

    snapshot = saved()
    observed = snapshot["rounds"][0]["tools"][0]
    assert snapshot["rounds"][-1]["text"] == split
    assert all(attempt["first_byte_us"] > 0 for attempt in snapshot["model_attempts"])
    assert len(observed["version"]) == 64 and observed["observed_utc"].endswith("Z")
    replayed, _ = cli([*session_args, "--resume"], [])
    assert citation in replayed
    for answer, repaired in (
        (split.replace("第 1 行", "第 2 行"), False),
        (split + f"\n依据 `./{SOURCE}:1`", False),
        (split + f"\n错误的写法是 `{SOURCE}:1.5`", True),
    ):
        stdout, _ = cli(starting, [
            ("POST", "/relay/v1/responses", initial_response),
            ("POST", "/relay/v1/responses", lambda payload: final_response(payload, answer)),
        ])
        assert (citation in stdout) == repaired
    stdout, _ = cli(starting, [("POST", "/relay/v1/responses", stream(completed([message(split)])))])
    assert "[citation]" not in stdout

    (root / SOURCE).write_text("new line\nexternal rewrite", encoding="utf-8")
    historical = (f"[historical citation] {SOURCE}:1 task=1 round=1 "
                  f"version={observed['version']} observed={observed['observed_utc']}")
    replayed, _ = cli([*session_args, "--resume"], [])
    assert historical in replayed and citation not in replayed

    follow_up = "保留记录，告诉我之前引用了哪里"

    def older_evidence(payload):
        assert payload["input"][0] == {"role": "user", "content": "Find the file"}
        assert payload["input"][-1] == {"role": "user", "content": follow_up}
        assert payload["input"][-2] == {"role": "assistant", "content": split}
        assert payload["input"][1] == native
        assert "task 1, round 1, version " + observed["version"] in payload["input"][3]["output"]
        assert set(tool["name"] for tool in payload["tools"]) == {"read", "search"}
        return completed([message(f"先前读取的是 `{SOURCE}:1`")]), 200, "text/event-stream"

    shown, _ = cli([*session_args, "--follow-up", follow_up], [("POST", "/relay/v1/responses", older_evidence)])
    assert shown.count(historical) == 2 and citation not in shown, shown
    assert shown.index("[task 1]") < shown.index(historical) < shown.index("[task 2]")
    replayed, _ = cli([*session_args, "--resume"], [])
    assert shown[shown.index("[task 1]"):] == replayed

    latest_item = {**item, "id": "fc_current", "call_id": "call_current"}
    location = f"文件：`{SOURCE}`\n行号：第 2 行"

    def current_evidence(payload):
        proposed, result = payload["input"][-2:]
        assert proposed["call_id"] == result["call_id"] == "call_current"
        assert "2: external rewrite" in result["output"]
        assert "task 3, round 4, version " in result["output"] and observed["version"] not in result["output"]
        return completed([message(location)]), 200, "text/event-stream"

    options = [*session_args, "--follow-up", "重新读取当前文件"]
    fresh = ("POST", "/relay/v1/responses", stream(completed([latest_item])))
    final = ("POST", "/relay/v1/responses", current_evidence)
    if os.name != "nt":
        requested, release = threading.Event(), threading.Event()

        def wait_for_interrupt(payload):
            result = current_evidence(payload)
            requested.set()
            assert release.wait(8)
            return result

        server.steps = [fresh, ("POST", "/relay/v1/responses", wait_for_interrupt)]
        server.errors = []
        with subprocess.Popen([executable, "--provider-config", config, *options], env=env,
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            try:
                assert requested.wait(5)
                process.send_signal(signal.SIGINT)
                stdout, stderr = process.communicate(timeout=5)
                assert process.returncode == 130 and b"[session] cancelled" in stdout, (stdout, stderr)
            finally:
                release.set()
                if process.poll() is None:
                    process.kill()
        assert not server.steps and not server.errors, server.errors
        cancelled = saved()
        assert cancelled["tasks"][-1]["question"] == options[-1] and not cancelled["tasks"][-1]["closed"]
        assert cancelled["dispatches"] == 2 and cancelled["rounds"][-1]["text"] == ""
        shown, _ = cli([*session_args, "--resume"], [final])
        restored = saved()
        assert restored["dispatches"] == 2 and restored["requests"] == cancelled["requests"] + 1
        assert restored["tasks"][-1]["id"] == cancelled["tasks"][-1]["id"]
        assert restored["tasks"][-1]["active_ms"] >= cancelled["tasks"][-1]["active_ms"] > 0
    else:
        shown, _ = cli(options, [fresh, final])
    assert f"[citation] {SOURCE}:2\n" in shown and f"[citation source] {SOURCE}:2 task=3 round=4" in shown

    incomplete = (response("response.output_text.delta", delta="未提交的预览")
                  + response("response.output_item.added", output_index=0, item={**item, "arguments": ""})
                  + completed([item]))
    broken, preview = cli(starting, [("POST", "/relay/v1/responses", stream(incomplete))], 1)
    assert "protocol_error" in broken and "dispatches=0" in broken
    assert "未提交的预览" not in broken and preview == "[preview] 未提交的预览\n"
    mismatch = response("response.created", response={"id": "resp_start"}) + completed([item], "resp_other")
    broken, _ = cli(starting, [("POST", "/relay/v1/responses", stream(mismatch))], 1)
    assert "could not be matched to the stream" in broken and "dispatches=0" in broken
    mismatch = (response("response.created", response={"id": "resp_start"})
                + response("response.output_item.done", output_index=0, item=item)
                + completed([{**item, "id": "fc_other"}], "resp_other"))
    broken, _ = cli(starting, [("POST", "/relay/v1/responses", stream(mismatch))], 1)
    assert "output item did not finish" in broken and "dispatches=0" in broken
    profiles["providers"]["relay"]["base_url"] = host + "/other/v1"
    config.write_text(json.dumps(profiles), encoding="utf-8")
    _, error = cli(["--workspace", root, "--session", "provider-contract", "--resume"], [], 2)
    assert "session scope changed" in error, error


if __name__ == "__main__":
    with TemporaryDirectory() as storage, ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            tests(sys.argv[1], Path(storage), server)
        finally:
            server.shutdown()
    print("Verified Chat Completions, Responses, catalog authentication, crash boundaries and provider scope.")
