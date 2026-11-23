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
        try:
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            pass

    def do_GET(self):
        self.handle_request("GET", None)

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        self.server.request_body_bytes = len(body)
        self.handle_request("POST", json.loads(body))


def stream(body):
    return lambda _: (body, 200, "text/event-stream")


def tests(executable, driver, base, server):
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

    def cli(arguments, steps, code=0, environment=env, program=executable, data=b""):
        server.steps = list(steps)
        server.errors = []
        outcome = subprocess.run([program, "--provider-config", config, *arguments], env=environment,
                                 input=data, capture_output=True, timeout=10)
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

    page = root / "app.py"
    page.write_bytes(b"def page(items, number, size):\n    start = number * size\n    return items[start:start + size]\n")
    calls = []
    counts = [
        {"input_tokens": 59000, "output_tokens": 10, "total_tokens": 59010,
         "input_tokens_details": {"cached_tokens": 58000, "provider_note": "omit this text"},
         "cache_creation_input_tokens": 24, "cache_read_input_tokens": 58000},
        {"input_tokens": 600, "output_tokens": 20, "input_tokens_details": {"cached_tokens": None}},
        {"input_tokens": 800, "output_tokens": -1, "output_tokens_details": {"reasoning_tokens": 5}},
        {"input_tokens": 900, "output_tokens": 25},
    ]

    def audit_round(index):
        def reply(payload):
            calls.append(server.request_body_bytes)
            arguments = {"path": "app.py"}
            if index == 1:
                arguments.update(version=payload["input"][-1]["output"].split("version=")[-1].strip(),
                                 old_text="number * size", new_text="(number - 1) * size")
            output = [message("分页偏移已修复。")] if index == 3 else [{
                "type": "function_call", "id": f"fc_audit_{index}", "call_id": f"call_audit_{index}",
                "name": "edit" if index == 1 else "read", "arguments": json.dumps(arguments),
            }]
            return response("response.completed", response={"id": f"resp_audit_{index}", "status": "completed",
                           "output": output, "usage": counts[index]}), 200, "text/event-stream"
        return ("POST", "/relay/v1/responses", reply)

    flags = ["--workspace", root, "--session", "usage-audit", "--write", "app.py"]
    cli([*flags, "--question", "只改 app.py。修复从 1 开始编号的分页起点。"],
        [audit_round(i) for i in range(4)], environment={**env, "LAY_TEST_INTERACTIVE": "1"}, program=driver, data=b"y\n")
    assert b"(number - 1) * size" in page.read_bytes()
    audit_db = database_dir / "session-usage-audit.sqlite3"
    with closing(sqlite3.connect(audit_db)) as database:
        attempts = json.loads(database.execute("SELECT snapshot FROM session").fetchone()[0])["model_attempts"]
    assert [a["request_number"] for a in attempts] == [1, 2, 3, 4]
    assert [a["body_bytes"] for a in attempts] == [a["uploaded_body_bytes"] for a in attempts] == calls
    assert [a["input_tokens"] for a in attempts] == [59000, 600, 800, 900]
    assert [a["output_tokens"] for a in attempts] == [10, 20, None, 25]
    assert attempts[0]["provider_usage"]["cache_read_input_tokens"] == 58000
    assert attempts[0]["provider_usage"]["input_tokens_details.cached_tokens"] == 58000
    assert attempts[1]["provider_usage"]["input_tokens_details.cached_tokens"] is None
    assert "input_tokens_details.cached_tokens" not in attempts[2]["provider_usage"]
    script = Path(__file__).resolve().parents[2] / "bench" / "usage_audit.py"
    report = json.loads(subprocess.check_output([sys.executable, "-B", script, audit_db]))
    assert report["tasks"][0]["totals"]["input_tokens"] == {"known_total": 61300, "requests_without_count": 0}
    assert report["tasks"][0]["totals"]["output_tokens"] == {"known_total": 55, "requests_without_count": 1}
    assert report["verified_cost"] is None
    followed = json.loads(subprocess.check_output([sys.executable, "-B", script, database_path]))
    assert [row["task"] for row in followed["tasks"]] == [1, 2, 3]
    assert [row["totals"]["input_tokens"]["known_total"] for row in followed["tasks"]] == [42, 21, 42]
    assert [row["totals"]["input_tokens"]["requests_without_count"] for row in followed["tasks"]] == [
        0, 0, int(os.name != "nt")]
    replayed, _ = cli([*flags, "--resume"], [])
    assert "end_turn requests=4" in replayed

    cap_env = {**env, "LAY_TEST_REQUEST_LIMIT": "2"}
    http_429 = ("POST", "/relay/v1/responses", lambda _: (b"", 429, "text/plain"))

    def budget_snapshot(session_id):
        with closing(sqlite3.connect(database_dir / f"session-{session_id}.sqlite3")) as database:
            result, = database.execute("SELECT snapshot FROM session").fetchone()
            phases = [row[0] for row in database.execute("SELECT phase FROM checkpoints ORDER BY revision")]
        return json.loads(result), phases

    # Kill after the second retry reached the relay but before it could reply.
    second_post = threading.Event()
    release = threading.Event()

    def wait_for_crash(_):
        second_post.set()
        release.wait(8)
        return b"", 200, "text/event-stream"

    cap_flags = ["--workspace", root, "--session", "retry-crash"]
    server.steps = [http_429, ("POST", "/relay/v1/responses", wait_for_crash)]
    server.errors = []
    process = subprocess.Popen([driver, "--provider-config", config, *cap_flags, "--question", "Retry"],
                               env=cap_env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert second_post.wait(6), server.errors
        saved, phases = budget_snapshot("retry-crash")
        assert saved["requests"] == 2 and saved["dispatches"] == 0
        assert phases == ["input", "request", "request_result", "request"]
        first, unconfirmed = saved["model_attempts"]
        assert (first["request_number"], first["attempt_state"], first["http_status"]) == (1, 2, 429)
        assert (unconfirmed["request_number"], unconfirmed["attempt_state"]) == (2, 0)
        assert unconfirmed["input_tokens"] is None and unconfirmed["output_tokens"] is None
        assert "uploaded_body_bytes" not in unconfirmed and "http_status" not in unconfirmed
        assert not server.steps and not server.errors
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)
        release.set()
    shown, _ = cli([*cap_flags, "--resume"], [], 1, cap_env, driver)
    assert "request_limit requests=2 dispatches=0" in shown
    assert "Unknown request receipts 1 · reserved, possibly unsent 1" in shown
    assert "Unknown input or output usage 2" in shown and "Remaining budget · requests 0" in shown
    assert budget_snapshot("retry-crash")[0]["model_attempts"] == saved["model_attempts"]
    report = json.loads(subprocess.check_output([sys.executable, "-B", script,
                                                 database_dir / "session-retry-crash.sqlite3"]))
    assert [attempt["attempt_state"] for attempt in report["tasks"][0]["attempts"]] == ["response", "reserved"]
    assert report["tasks"][0]["totals"]["input_tokens"] == {"known_total": None, "requests_without_count": 2}

    # A committed reservation can predate the transfer itself.
    cap_flags = ["--workspace", root, "--session", "retry-before-send"]
    server.steps, server.errors = [], []
    process = subprocess.Popen([driver, "--provider-config", config, *cap_flags, "--question", "Reserve"],
                               env={**cap_env, "LAY_TEST_PAUSE": "after_request_1"}, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    marker = threading.Event()
    def watch_reservation():
        for line in process.stdout:
            if b"[checkpoint] after_request_1" in line:
                marker.set()
                return

    monitor = threading.Thread(target=watch_reservation)
    monitor.start()
    try:
        assert marker.wait(6), "request reservation checkpoint was not reached"
        saved, phases = budget_snapshot("retry-before-send")
        assert saved["requests"] == 1 and phases == ["input", "request"]
        assert saved["model_attempts"][0]["attempt_state"] == 0 and not server.errors
    finally:
        if process.poll() is None:
            process.kill()
        monitor.join(5)
        process.communicate(timeout=5)
    shown, _ = cli([*cap_flags, "--resume"], [("POST", "/relay/v1/responses", stream(completed([message("Recovered")])))],
                   environment=cap_env, program=driver)
    assert "end_turn requests=2 dispatches=0" in shown
    assert "Unknown request receipts 1 · reserved, possibly unsent 1" in shown

    # A write failure on reservation 2 stops issuance and leaves only the first receipt.
    cap_flags = ["--workspace", root, "--session", "retry-save-failure"]
    shown, _ = cli([*cap_flags, "--question", "Retry"], [http_429], 1,
                   {**cap_env, "LAY_TEST_FULL": "before_request_2"}, driver)
    assert "storage_error" in shown and "disk is full" in shown
    saved, phases = budget_snapshot("retry-save-failure")
    assert saved["requests"] == 1 and phases == ["input", "request", "request_result"]
    assert saved["model_attempts"][0]["http_status"] == 429

    print("Verified a two-POST crash cap, an unsent reservation, and save failure before retry.")

    def proposal(identifier, name, **arguments):
        return {"type": "function_call", "id": "fc_" + identifier, "call_id": identifier, "name": name,
                "arguments": json.dumps(arguments)}

    def repeated_session(name, steps, *, pause=False, extra=(), environment=env, data=b""):
        flags = ["--workspace", root, "--session", name, *extra]
        shown, _ = cli([*flags, "--question", "Compare host observations"],
                       [("POST", "/relay/v1/responses", step) for step in steps],
                       int(pause), environment, driver, data)
        saved, phases = budget_snapshot(name)
        tools = [tool for round in saved["rounds"] for tool in round["tools"]]
        assert ("[session] repeated_file_failure" in shown) == pause, shown
        assert saved["requests"] == len(steps) == phases.count("request") == phases.count("request_result")
        assert saved["dispatches"] == phases.count("dispatch") == sum(tool["dispatched"] for tool in tools)
        return flags, shown, tools

    def read_step(identifier, path="binary.txt", **arguments):
        return stream(completed([proposal(identifier, "read", path=path, **arguments)]))

    answer = stream(completed([message("Finished")]))
    (root / "binary.txt").write_bytes(b"\x00v1")
    (root / "other.txt").write_bytes(b"\x00v2")
    first = read_step("first")
    valid = read_step("new-fact", SOURCE)
    explicit_defaults = proposal("second", "read", line_count=120, path="binary.txt", start_line=1)
    remainder = proposal("unreached", "read", path=SOURCE)
    flags, shown, tools = repeated_session("same-file-failure",
        [first, stream(completed([explicit_defaults, remainder]))], pause=True)
    assert "Calls first and second repeated read on binary.txt" in shown and "same verified file version" in shown
    assert tools[0]["version"] == tools[1]["version"] and tools[0]["file_failure"] == tools[1]["file_failure"]
    assert tools[2]["status"] == 3 and not tools[2]["dispatched"]
    no_key = {name: value for name, value in env.items() if name != KEY_VARIABLE}
    inspected, _ = cli([*flags, "--resume", "--inspect"], [], environment=no_key)
    assert "Task 1 receipt · repeated_file_failure" in inspected and "Calls first and second" in inspected
    assert budget_snapshot("same-file-failure")[0]["dispatches"] == 2
    # An explicit resume can check a file that the user has changed.
    (root / "binary.txt").write_text("readable now", encoding="utf-8")
    shown, _ = cli([*flags, "--resume"], [("POST", "/relay/v1/responses", read_step("recheck")),
                                         ("POST", "/relay/v1/responses", answer)])
    assert "end_turn requests=4 dispatches=3" in shown
    (root / "binary.txt").write_bytes(b"\x00v1")

    def change_version(payload):
        (root / "binary.txt").write_bytes(b"\x00v2")
        return read_step("second")(payload)

    for name, steps in (
        ("different-version", [first, change_version, answer]),
        ("different-parameters", [first, read_step("second", line_count=119), answer]),
        ("different-file", [first, read_step("second", "other.txt"), answer]),
        ("between-observations", [first, valid, read_step("second"), answer]),
        ("unknown-file-version", [read_step("first", "missing.txt"), read_step("second", "missing.txt"), answer]),
        ("excluded-file", [read_step("first", "other.txt"), read_step("second", "other.txt"), answer]),
    ):
        extra = ("--exclude", "other.txt") if name == "excluded-file" else ()
        _, shown, tools = repeated_session(name, steps, extra=extra)
        if name in ("different-version", "different-file"):
            assert tools[0]["version"] and tools[1]["version"] and tools[0]["version"] != tools[1]["version"]
        if name in ("unknown-file-version", "excluded-file"):
            assert all("file_failure" not in tool for tool in tools)

    original = (root / SOURCE).read_bytes()
    def edit_error(identifier):
        def reply(payload):
            if identifier == "first":
                reply.version = payload["input"][-1]["output"].split("version=")[-1].strip()
            tool = proposal(identifier, "edit", path=SOURCE, version=reply.version,
                            old_text="missing fragment", new_text="replacement")
            return stream(completed([tool]))(payload)
        return reply

    edit_first = edit_error("first")
    def edit_second(payload):
        reply = edit_error("second")
        reply.version = edit_first.version
        return reply(payload)

    _, shown, tools = repeated_session("same-edit-failure", [valid, edit_first, edit_second], pause=True,
                                      extra=("--write", SOURCE))
    assert "Calls first and second repeated edit" in shown
    assert tools[1]["file_failure"] == tools[2]["file_failure"] and not tools[1]["dispatched"] and not tools[2]["dispatched"]
    assert (root / SOURCE).read_bytes() == original

    def command_step(identifier):
        return stream(completed([proposal(identifier, "run", program=sys.executable,
            args=["-c", "import sys; sys.stderr.write('same stderr'); sys.exit(7)"])]))

    run_env = {**env, "LAY_TEST_INTERACTIVE": "1"}
    _, _, tools = repeated_session("identical-stderr", [command_step("first"), command_step("second"), answer],
                                   extra=("--allow-run",), environment=run_env, data=b"y\ny\n")
    assert all(tool["dispatched"] and tool["verification"]["stderr"]["head"] == "same stderr" for tool in tools)
    _, shown, tools = repeated_session("denied-command", [command_step("first"), command_step("second"), answer],
                                       extra=("--allow-run",), environment=run_env, data=b"n\n")
    assert shown.count("Run this command? [y/N]") == 1 and all(not tool["dispatched"] for tool in tools)
    print("Verified two attributable file pauses and eight counterexamples with no false pause.")

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
            tests(sys.argv[1], sys.argv[2], Path(storage), server)
        finally:
            server.shutdown()
    print("Verified Chat Completions, Responses, catalog authentication, crash boundaries and provider scope.")
