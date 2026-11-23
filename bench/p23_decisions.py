"""Check decision provenance on 10, 50 and 100 real persisted editor tasks."""

import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
from urllib.parse import urlsplit, urlunsplit

from live_quality import context
from mixed_history import trace
from tui_performance import measurements

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "e2e"))
from supplement_live import answer, history, relay, results, send, tool_reply
from tui import console
from tui_workspace import submit


PROTOCOL = "responses"
TARGETS = (10, 50, 100)
ORIGINAL = "Keep the old settings key because the kiosk importer still requires it."
WITHDRAWAL = "I withdraw the instruction to keep the old settings key after the kiosk importer upgrade."
SPECULATION = "My guess is that an audit policy might also require the key."
ASK = {
    "original": "Why did we previously keep the old settings key? Name the source task number and quote the user's exact sentence.",
    "withdrawal": "Did the user later withdraw the instruction to keep the old settings key? Answer yes or no, name the source task number and quote the user's exact sentence.",
}
REAPPROVAL = "Check whether the earlier refusal exists and whether it automatically changed."
EXPECTED = {"original": (2, ORIGINAL), "withdrawal": (5, WITHDRAWAL)}
CONFIG = "config.txt"
ARCHIVE = "archive.txt"
BASE_CONFIG = b"legacy_settings=keep\n"
NEW_CONFIG = "legacy_settings=remove\n"
FIRST_QUESTION = "Read the compatibility note and the current settings."
FILL = "".join(f"archived release item {n:02d} has no bearing on the settings key\n" for n in range(20))
CONTEXT_ERROR = b'{"error":{"code":"context_length_exceeded","message":"fixture smaller context"}}'
REFERENCE_HEADER = "[Host selected decision references]\n"
REFERENCE_FLAGS = ("--quote-task", "2", "--current-decision-task", "5")


def prompt(task):
    fixed = {1: FIRST_QUESTION, 2: ORIGINAL, 3: "Review a possible change to the old settings key.",
             4: "What other possible motives did you consider?", 5: WITHDRAWAL}
    return fixed.get(task, f"Read the note and settings for the routine release check {task:03d}.")


def version(result):
    return re.search(r"^version=(\S+)$", result, re.M)[1]


def sha(value):
    return hashlib.sha256(value).hexdigest()


def user_inputs(items):
    questions, summaries = [], []
    for item in items:
        if item.get("role") != "user":
            continue
        if isinstance(item["content"], str):
            if not item["content"].startswith(REFERENCE_HEADER):
                questions.append(item["content"])
        else:
            for part in item["content"]:
                if part["text"].startswith("Host facts from older workspace rounds."):
                    summaries.append(part["text"])
                else:
                    questions.append(part["text"])
    return questions, summaries


def unchanged_groups(items):
    calls = [row["call_id"] for row in items if row.get("type") == "function_call"]
    returned = list(results(items, PROTOCOL))
    assert calls == returned and len(set(calls)) == len(calls)
    return len(calls)


def meaningful_reply(kind, reply):
    task, quote = EXPECTED[kind]
    return (quote in reply and bool(re.search(rf"\btask[\s*#:`]{{0,8}}0*{task}\b", reply, re.I))
            and (kind != "withdrawal" or bool(re.search(r"\byes\b", reply, re.I)))
            and "audit policy" not in reply.lower())


def reference_fields():
    def source(task, text):
        return {"task": task, "message": f"task-{task}-question", "version": sha(text.encode()), "role": "user", "text": text}

    return {"earlier": source(2, ORIGINAL), "current": source(5, WITHDRAWAL)}


class RecordedDecisions:
    def __init__(self, using_references):
        self.task = 0
        self.case = None
        self.original_version = None
        self.recent_version = None
        self.config_version = None
        self.retry = False
        self.using_references = using_references

    def plan(self, _, payload):
        items = history(payload, PROTOCOL)
        inputs, summaries = user_inputs(items)
        observed = results(items, PROTOCOL)
        assert ORIGINAL not in [row.get("content") for row in items if row.get("role") == "assistant"]
        selected = None
        if self.case and self.using_references:
            capsule, = [item["content"] for item in items if item.get("role") == "user" and
                        isinstance(item["content"], str) and item["content"].startswith(REFERENCE_HEADER)]
            selected = json.loads(capsule.removeprefix(REFERENCE_HEADER))
            assert selected == reference_fields()
        if self.case == "authority":
            assert inputs[-1] == REAPPROVAL
            if "old-denial-again" not in observed:
                return self.edit("old-denial-again")
            assert observed["old-denial-again"].endswith("edit denied by user")
            return send(answer(PROTOCOL, "The earlier refusal still applies."))
        if self.case in ASK:
            assert inputs[-1] == ASK[self.case]
            if not self.retry:
                self.retry = True
                return send(CONTEXT_ERROR, 400)
            assert summaries and "user denied this action" in "\n".join(summaries)
            serialized = json.dumps(items)
            assert self.original_version in serialized and self.recent_version in serialized
            assert unchanged_groups(items) >= 2
            task, quote = EXPECTED[self.case]
            assert inputs[:self.task] == [prompt(i) for i in range(1, self.task + 1)]
            assert inputs[task - 1] == quote and SPECULATION not in inputs
            source = selected["earlier" if self.case == "original" else "current"] if selected else {
                "task": task, "text": inputs[task - 1]}
            response = f"Task {source['task']}. Quote: {source['text']}"
            return send(answer(PROTOCOL, ("Yes. " if self.case == "withdrawal" else "") + response))

        task = self.task
        assert inputs[-1] == prompt(task)
        if task in (2, 4, 5):
            return send(answer(PROTOCOL, SPECULATION if task == 4 else f"Recorded task {task:03d}."))
        read_id = f"archive-{task}"
        config_id = f"config-{task}"
        if read_id not in observed:
            return send(tool_reply(PROTOCOL, [(read_id, "read", {"path": ARCHIVE, "line_count": 30}),
                                              (config_id, "read", {"path": CONFIG})]))
        if task == 1:
            self.original_version = version(observed[read_id])
            self.config_version = version(observed[config_id])
        if task == 3 and "denied-change" not in observed:
            return self.edit("denied-change")
        if task == 3:
            assert observed["denied-change"].endswith("edit denied by user")
        if task >= 6:
            self.recent_version = version(observed[read_id])
            assert self.recent_version != self.original_version
        return send(answer(PROTOCOL, f"Recorded task {task:03d}."))

    def edit(self, identity):
        return send(tool_reply(PROTOCOL, [(identity, "edit", {"path": CONFIG, "version": self.config_version,
                                                             "old_text": BASE_CONFIG.decode(), "new_text": NEW_CONFIG})]))


def refusal(screen):
    screen.key("\t", "空回车拒绝")
    screen.send("\r")


def add_tasks(fixture, script, first, final):
    flags = ("--resume",) if first > 1 else ()
    with console(fixture.cli, ["--tui", *fixture.args, "--mono", *flags], 30, 120, fixture.env) as terminal:
        terminal.expect("已完成" if flags else "等待任务")
        for task in range(first, final + 1):
            script.task = task
            script.case = None
            if task == 6:
                (fixture.root / ARCHIVE).write_text("Updated compatibility note.\n" + FILL, encoding="utf-8")
            terminal.text = ""
            submit(terminal, prompt(task))
            if task == 3:
                terminal.expect("待批准 · 编辑 " + CONFIG)
                refusal(terminal)
            terminal.expect(SPECULATION if task == 4 else f"Recorded task {task:03d}.", timeout=10)
            terminal.expect("已完成", timeout=10)


def state_copy(fixture, base, name):
    env = {**fixture.env}
    if os.name == "nt":
        env["LOCALAPPDATA"] = str(base / name)
        database = base / name / "LayCode" / "sessions" / fixture.db.name
    else:
        env["XDG_STATE_HOME"] = str(base / name)
        database = base / name / "lay-code" / "sessions" / fixture.db.name
    database.parent.mkdir(parents=True)
    shutil.copyfile(fixture.db, database)
    env["LAY_BENCHMARK_TRACE"] = str(base / (name + ".tsv"))
    return env, database


def printed_command(app, args, env, timeout=18):
    process = subprocess.run([app, *args], capture_output=True, stdin=subprocess.DEVNULL, env=env, timeout=timeout)
    return process.returncode, (process.stdout + process.stderr).decode("utf-8")


def decisions(fixture, script, server, base, args, count, expected, name, case):
    assert count == len(expected["tasks"]) and expected["finished"]
    env, database = state_copy(fixture, base, name)
    prompt_ = REAPPROVAL if case == "authority" else ASK[case]
    script.case, script.retry = case, False
    previous = len(server.requests)
    extra = ["--follow-up", prompt_] + ([] if case == "authority" else ["--no-write"])
    if args.references:
        extra += REFERENCE_FLAGS
    code, output = printed_command(fixture.cli, [*fixture.args, *extra], env)
    assert code == 0, output
    blocks = server.requests[previous:]
    saved = context(database)
    observed = saved["tasks"][-1]
    assert observed["question"] == prompt_ and saved["finished"] and len(blocks) == 2
    assert saved["requests"] == expected["requests"] + 2 and saved["dispatches"] == expected["dispatches"]
    assert (fixture.root / CONFIG).read_bytes() == BASE_CONFIG
    reqs = saved["model_attempts"][expected["requests"]:]
    if args.references:
        assert observed["decision_reference"] == {
            key: {k: v for k, v in value.items() if k not in ("text", "role")} for key, value in reference_fields().items()}
        assert all(row["template_version"] == 6 for row in reqs)
    assert [len(block) for block in blocks] == [row["uploaded_body_bytes"] for row in reqs]
    outcome = {"extra_requests": len(blocks), "body_bytes": [len(block) for block in blocks],
               "first_retained_round": [row["first_round"] for row in reqs], "request_hashes": [sha(block) for block in blocks]}
    if case == "authority":
        (record,) = saved["rounds"][observed["first_round"]]["tools"]
        assert record["id"] == "old-denial-again" and not record["dispatched"] and record["status"] == 2
        assert "edit denied by user" == record["result"] and record["edit"].get("approved") is not True
        outcome["dispatches"] = 0
    else:
        assert reqs[-1]["first_round"] > reqs[0]["first_round"]
        assert meaningful_reply(case, saved["rounds"][-1]["text"])
        outcome["source_task"], outcome["exact_user_quote"] = EXPECTED[case]
    metrics = Path(env["LAY_BENCHMARK_TRACE"])
    if args.metrics:
        prepared = trace(metrics, count + 1, "projection")
        assert [size for size, _ in prepared] == outcome["body_bytes"]
        outcome["projection_ms"] = [elapsed for _, elapsed in prepared]
    return outcome


def read_only_online(fixture, base, options, count, expected, case):
    profile = json.loads(options.provider_config.read_text(encoding="utf-8-sig"))["providers"][options.provider]
    assert profile["protocol"] == PROTOCOL
    parts = urlsplit(profile["base_url"])
    path = parts.path.rstrip("/").removesuffix("/responses") or "/v1"
    endpoint = urlunsplit((parts.scheme, parts.netloc, path + "/responses", "", ""))
    assert parts.scheme == "https" and profile["api_key_env"]
    model = options.model or profile["model"]
    env, database = state_copy(fixture, base, f"live-{count}-{case}")
    # Rebind only the disposable copy to the real provider. The recorded user turns and tool evidence stay intact.
    with closing(sqlite3.connect(database)) as store, store:
        scope = json.loads(store.execute("SELECT scope FROM session").fetchone()[0])
        scope.update(provider=f"responses|{endpoint}|{profile['api_key_env']}", model=model)
        store.execute("UPDATE session SET scope=?", (json.dumps(scope, separators=(",", ":"), ensure_ascii=False),))
    argv = fixture.args[:]
    argv[argv.index("--provider-config") + 1] = str(options.provider_config)
    argv += ["--provider", options.provider, "--model", model, "--follow-up", ASK[case], "--no-write"]
    if options.references:
        argv += REFERENCE_FLAGS
    try:
        code, output = printed_command(fixture.cli, argv, env, timeout=120)
    except subprocess.TimeoutExpired:
        code, output = 124, "Live attempt timed out."
    saved = context(database)
    assert saved["tasks"][:-1] == expected["tasks"] and len(saved["tasks"]) == count + 1, output[-300:]
    dispatched = [tool["name"] for round_ in saved["rounds"][len(expected["rounds"]):] for tool in round_["tools"]
                  if tool["dispatched"]]
    assert set(dispatched) <= {"read", "search"} and (fixture.root / CONFIG).read_bytes() == BASE_CONFIG
    assert saved["dispatches"] - expected["dispatches"] == len(dispatched)
    response = saved["rounds"][-1]["text"] if code == 0 and saved["finished"] else ""
    attempts = saved["model_attempts"][expected["requests"]:]
    passed = meaningful_reply(case, response)
    print(f"Live {count}/{case}: {'pass' if passed else 'fail'}, {len(attempts)} requests, "
          f"{len(dispatched)} read-only calls", file=sys.stderr, flush=True)
    return {"provider": options.provider, "model": model, "extra_requests": len(attempts), "exit_code": code,
            "body_bytes": [row["body_bytes"] for row in attempts], "read_only_dispatches": dispatched,
            "usage": [{key: row.get(key) for key in ("input_tokens", "output_tokens")} for row in attempts],
            "first_retained_round": [row["first_round"] for row in attempts],
            "passed": passed, "reply": response}


def run(options, base):
    script = RecordedDecisions(options.references)
    app = str(options.executable)
    report = {}
    with relay(base, "decisions", app, app, PROTOCOL, script.plan, None) as (fixture, server):
        fixture.args += ["--write", CONFIG]
        (fixture.root / CONFIG).write_bytes(BASE_CONFIG)
        (fixture.root / ARCHIVE).write_text("Original compatibility note.\n" + FILL, encoding="utf-8")
        for first, count in zip((1, 11, 51), TARGETS):
            add_tasks(fixture, script, first, count)
            archived = context(fixture.db)
            previous = len(server.requests)
            reopens = []
            for _ in range(10):
                start = time.perf_counter_ns()
                with console(app, ["--tui", *fixture.args, "--resume", "--mono"], 30, 120, fixture.env) as terminal:
                    terminal.expect("已完成", timeout=10)
                    terminal.key("ready probe", "> ready probe")
                    reopens.append((time.perf_counter_ns() - start) / 1e6)
            assert len(server.requests) == previous and context(fixture.db) == archived
            evidence = {}
            for case in ASK:
                records = [decisions(fixture, script, server, base, options, count, archived, f"{count}-{case}-{trial}", case)
                           for trial in range(2)]
                for key in ("request_hashes", "first_retained_round", "body_bytes", "source_task", "exact_user_quote"):
                    assert records[0][key] == records[1][key]
                evidence[case] = records
            authority = decisions(fixture, script, server, base, options, count, archived, f"{count}-authority", "authority")
            report[str(count)] = {"history_requests": archived["requests"], "archived_database_bytes": fixture.db.stat().st_size,
                                  "historical_read_versions": [script.original_version, script.recent_version],
                                  "tui_reopen_to_draft_echo": measurements(reopens), "decisions": evidence,
                                  "recorded_denial": authority}
            if options.provider_config:
                report[str(count)]["online"] = {case: read_only_online(fixture, base, options, count, archived, case) for case in ASK}
            print(f"Verified {count} history tasks", file=sys.stderr, flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--metrics", action="store_true")
    parser.add_argument("--references", action="store_true")
    parser.add_argument("--provider-config", type=Path)
    parser.add_argument("--provider", default="passion")
    parser.add_argument("--model")
    options = parser.parse_args()
    options.executable = options.executable.resolve(strict=True)
    with TemporaryDirectory(prefix="lay-p23-") as directory:
        sizes = run(options, Path(directory))
    print(json.dumps({"milestone": "P23", "executable_sha256": sha(options.executable.read_bytes()),
                      "protocol": PROTOCOL, "explicit_references": options.references, "sizes": sizes},
                     indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
