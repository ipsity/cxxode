"""Serve and inspect the isolated RC8 Windows Terminal acceptance fixture."""

import argparse
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import sqlite3
import sys
import time


SESSION = "p10-rc8"
ORIGINAL = "empty_config = reject"
EDITED = "empty_config = allow"
LONG_LINE = " # " + "x" * 800 + " LONG-DIFF-END\n"
HOLD = "hold check.py"
PASTE = "y\nsecond line of paste\n"


class Relay(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        if self.path != "/v1/responses":
            self.send_error(404)
            return
        items = json.loads(self.rfile.read(int(self.headers["Content-Length"])))["input"]
        task = sum(item.get("role") == "user" for item in items)
        outputs = {item["call_id"]: item["output"] for item in items if item.get("type") == "function_call_output"}
        action, reply = self.reply(task, outputs)
        with self.server.ledger.open("a", encoding="utf-8") as ledger:
            ledger.write(json.dumps({"task": task, "reply": action}) + "\n")
        print(f"task={task}  reply={action}", flush=True)
        if action == "edit":
            time.sleep(4)
        frame = {"type": "response.completed", "response": {"status": "completed", "output": [reply]}}
        data = f"data: {json.dumps(frame)}\n\n".encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    @staticmethod
    def reply(task, outputs):
        if task == 2:
            if "p10-read" not in outputs:
                return "read", call("read", {"path": "config.txt"})
            if "p10-edit" not in outputs:
                version = re.search(r"^version=(\S+)$", outputs["p10-read"], re.M)
                if not version:
                    return answer(task, "Fixture error: config.txt version is missing.")
                return "edit", call("edit", {"path": "config.txt", "version": version[1],
                                             "old_text": ORIGINAL, "new_text": EDITED})
            if "p10-run" not in outputs:
                return "run", call("run", {"program": Path(sys.executable).as_posix(),
                                           "args": ["-B", HOLD, "argument with spaces"], "timeout_ms": 75000})
            return answer(task, "Fixture command finished before it was cancelled.")
        if task == 3:
            if "command cancelled" in outputs.get("p10-run", ""):
                return answer(task, "Previous cancellation found. No new tool call.")
            return answer(task, "Fixture error: previous cancellation missing.")
        return answer(task, "Chinese input test received.")


def call(name, arguments):
    return {"type": "function_call", "id": f"fc_p10_{name}", "call_id": f"p10-{name}",
            "name": name, "arguments": json.dumps(arguments)}


def answer(task, text):
    return "message", {"type": "message", "id": f"msg_p10_{task}", "role": "assistant",
                       "content": [{"type": "output_text", "text": text}]}


def serve(directory):
    directory.mkdir(parents=True)
    workspace = directory / "workspace"
    workspace.mkdir()
    (workspace / "config.txt").write_text(ORIGINAL + LONG_LINE, encoding="utf-8", newline="\n")
    (workspace / HOLD).write_text(
        'from pathlib import Path\nimport sys\nimport time\n'
        'assert sys.argv[1:] == ["argument with spaces"]\n'
        'with Path("run-started.txt").open("a") as record:\n    record.write("started\\n")\n'
        'time.sleep(60)\nPath("run-finished.txt").touch()\n', encoding="utf-8")
    (directory / "paste-sample.txt").write_text(PASTE, encoding="utf-8", newline="\r\n")
    with ThreadingHTTPServer(("127.0.0.1", 0), Relay) as server:
        server.ledger = directory / "requests.jsonl"
        server.ledger.touch()
        profile = {"default_provider": "p10-loopback", "providers": {"p10-loopback": {
            "protocol": "responses", "model": "p10-manual", "api_key_env": None,
            "base_url": f"http://127.0.0.1:{server.server_port}/v1",
        }}}
        (directory / "provider.json").write_text(json.dumps(profile, indent=2), encoding="utf-8")
        print(f"READY {directory}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


def status(directory):
    ledger = directory / "requests.jsonl"
    requests = [json.loads(line) for line in ledger.read_text(encoding="utf-8").splitlines()]
    workspace = directory / "workspace"
    edits = (workspace / "config.txt").read_text(encoding="utf-8")
    file_state = {ORIGINAL + LONG_LINE: "original", EDITED + LONG_LINE: "edited"}.get(edits, "unexpected")
    start = workspace / "run-started.txt"
    db = directory / "state" / "LayCode" / "sessions" / f"session-{SESSION}.sqlite3"
    saved = {}
    if db.is_file():
        with closing(sqlite3.connect(f"{db.as_uri()}?mode=ro", uri=True)) as conn:
            saved = json.loads(conn.execute("SELECT snapshot FROM session").fetchone()[0])
    tools = [tool for round_ in saved.get("rounds", []) for tool in round_["tools"]]
    stop_names = {0: "end_turn", 2: "cancelled", 9: "time_limit", 11: "effect_unknown"}
    return {"requests": len(requests), "request_trace": requests, "tasks": len(saved.get("tasks", [])),
            "task_states": [stop_names.get(task["stop"], task["stop"]) for task in saved.get("tasks", [])],
            "config": file_state, "edit_dispatches": sum(tool["dispatched"] for tool in tools if tool["name"] == "edit"),
            "run_dispatches": sum(tool["dispatched"] for tool in tools if tool["name"] == "run"),
            "process_starts": len(start.read_text(encoding="utf-8").splitlines()) if start.is_file() else 0,
            "process_finished": (workspace / "run-finished.txt").is_file(),
            "run_results": [tool["result"].split("\n", 1)[0] for tool in tools if tool["name"] == "run"],
            "last_reply": next((round_["text"] for round_ in reversed(saved.get("rounds", [])) if round_["text"]), "")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("serve", "status"))
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    if args.action == "serve":
        if directory.exists():
            parser.error("Choose a new test directory. Existing records are preserved.")
        serve(directory)
    else:
        print(json.dumps(status(directory), indent=2))


if __name__ == "__main__":
    main()
