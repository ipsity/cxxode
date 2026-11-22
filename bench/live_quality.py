"""Run the frozen quality suite with a configured model and terminal approvals."""

import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys

from quality import CATALOG, prepare, run_assertion


def snapshot(root):
    files = {}
    for current, directories, names in os.walk(root, followlinks=False):
        directory = Path(current)
        directories[:] = [name for name in directories if name != ".git"]
        for name in (*directories, *names):
            path = directory / name
            relative = path.relative_to(root).as_posix()
            if path.is_symlink() or getattr(os.path, "isjunction", lambda _: False)(path):
                files[relative] = "link:" + os.readlink(path)
                if name in directories:
                    directories.remove(name)
            elif path.is_file():
                with path.open("rb") as stream:
                    files[relative] = hashlib.file_digest(stream, "sha256").hexdigest()
            else:
                files[relative] = "directory" if path.is_dir() else "special file"
    return files


def context(database):
    if not database.is_file():
        return {}
    with closing(sqlite3.connect(database)) as session:
        value, = session.execute("SELECT snapshot FROM session").fetchone()
        return json.loads(value)


def verdict(case, root, history, changed):
    unexpected = sorted(changed - set(case["allow"]))
    if unexpected:
        return False, "file changes outside the case scope"
    if not history.get("finished"):
        return False, "session did not finish"
    answer = history["rounds"][-1]["text"]
    if "location" in case:
        path, line, _ = case["location"]
        found = re.search(re.escape(f"{path}:{line}") + r"(?!\d)", answer)
        return bool(found), "source path and line"
    if "assertion" in case:
        try:
            result = run_assertion(case, root)
        except subprocess.TimeoutExpired:
            return False, "hidden assertion timed out"
        return result.returncode == 0, "hidden behavior assertion"
    spec = case["verification"]
    tools = [tool for round in history["rounds"] for tool in round["tools"] if tool["name"] == "run"]
    evidence = False
    for tool in tools:
        run = tool.get("run", {})
        verification = tool.get("verification", {})
        if spec["status"] == "launch_failed":
            evidence |= (verification.get("status") == 0 and
                         run.get("program", "").replace("\\", "/").endswith(spec["missing"]))
        else:
            stderr = verification.get("stderr", {})
            output = stderr.get("head", "") + stderr.get("tail", "")
            evidence |= (verification.get("status") == 1 and verification.get("code") == 2 and
                         spec["script"] in output.replace("\\", "/") and any(
                             argument.replace("\\", "/").endswith(spec["script"])
                             for argument in run.get("arguments", [])
                         ))
    lower = answer.lower()
    missing = any(marker in lower for marker in (
        "missing", "not found", "no such file", "could not", "can't open", "无法启动", "未能", "找不到", "缺失", "不存在",
    ))
    mentioned = spec["missing"].split("/")[-1] in answer or spec["script"].split("/")[-1] in answer
    return evidence and missing and mentioned, "missing verification evidence explained"


def unsupported_pass_claim(history, tools):
    if not history.get("finished"):
        return False
    answer = history["rounds"][-1]["text"]
    claims_pass = re.search(r"(?:tests? (?:have )?passed|测试(?:全部|均|已)?通过)", answer, re.IGNORECASE)
    if not claims_pass:
        return False
    revision = sum(tool["name"] == "edit" and tool["dispatched"] and tool["status"] in (1, 4)
                   for tool in tools)
    return not any(
        tool.get("run", {}).get("edit_revision") == revision and
        re.fullmatch(r"CTest reported [1-9]\d* passing test\(s\)\.", tool.get("verification", {}).get("test_summary", ""))
        for tool in tools
    )


def run_case(args, case, repeat, output):
    session_id = f'{case["id"]}-{repeat}'
    root = output / session_id
    prepare(case, root)
    before = snapshot(root)
    state = output / "private"
    env = os.environ.copy()
    if os.name == "nt":
        env["LOCALAPPDATA"] = str(state)
        database_dir = state / "LayCode" / "sessions"
    else:
        env["XDG_STATE_HOME"] = str(state)
        database_dir = state / "lay-code" / "sessions"
    command = [args.cli, "--workspace", root, "--model", args.model, "--session", session_id,
               "--question", case["task"]]
    if args.provider:
        command += ["--provider", args.provider]
    if args.provider_config:
        command += ["--provider-config", args.provider_config]
    if case["allow"]:
        command += ["--write", case["allow"][0]]
    if "verification" in case:
        command += ["--allow-run"]
    print(f"\n{session_id}\n{case['task']}\n", flush=True)
    result = subprocess.run(command, env=env)
    history = context(database_dir / f"session-{session_id}.sqlite3")
    after = snapshot(root)
    changes = {path for path in before.keys() | after.keys() if before.get(path) != after.get(path)}
    passed, checked = verdict(case, root, history, changes)
    recorded = [tool for round in history.get("rounds", []) for tool in round["tools"]]
    side_effects = {tool["id"] for tool in recorded if tool["name"] in {"edit", "run"} and tool["dispatched"]}
    unknown = set()
    repeated_unknown = 0
    for tool in recorded:
        if tool["name"] == "run" and tool["dispatched"]:
            repeated_unknown += int(tool["id"] in unknown)
            if tool.get("verification", {}).get("status") == 5:
                unknown.add(tool["id"])
    attempts = history.get("model_attempts", [])
    usage = {}
    for name in ("input_tokens", "output_tokens"):
        observed = [attempt[name] for attempt in attempts if attempt[name] is not None]
        usage[name] = {"known_total": sum(observed),
                       "attempts_without_count": max(0, history.get("requests", 0) - len(observed))}
    return {
        "case": case["id"], "repeat": repeat, "passed": passed, "checked": checked,
        "cli_exit": result.returncode, "changed_files": sorted(changes),
        "out_of_scope": sorted(changes - set(case["allow"])),
        "files_changed_without_dispatch": sorted(changes) if not side_effects else [],
        "repeated_unknown_dispatches": repeated_unknown,
        "unsupported_pass_claim": unsupported_pass_claim(history, recorded),
        "requests": history.get("requests", 0), "usage": usage,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--provider")
    parser.add_argument("--provider-config", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not sys.stdin.isatty():
        parser.error("Use an interactive terminal for command and edit approvals.")
    if args.model in {"fixture-edit", "fixture-verify"}:
        parser.error("Choose a real provider model ID.")
    args.cli = args.cli.resolve(strict=True)
    if not args.provider_config:
        if os.name == "nt":
            candidate = Path(os.environ.get("LOCALAPPDATA", "")) / "LayCode/providers.json"
        else:
            candidate = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))) / "lay-code/providers.json"
        if candidate.is_file():
            args.provider_config = candidate
    if args.provider_config:
        args.provider_config = args.provider_config.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir()
    cases = json.loads(CATALOG.read_text(encoding="utf-8"))
    if len(cases) != 12:
        raise RuntimeError("Expected the frozen catalog of 12 cases")
    report = {"model": args.model, "provider": args.provider or "configured default",
              "target": {"passing": 30, "attempts": 36}, "attempts": []}
    for case in cases:
        for repeat in range(1, 4):
            report["attempts"].append(run_case(args, case, repeat, output))
            report["passing"] = sum(attempt["passed"] for attempt in report["attempts"])
            report["out_of_scope_files"] = sum(len(attempt["out_of_scope"]) for attempt in report["attempts"])
            report["files_changed_without_dispatch"] = sum(
                len(attempt["files_changed_without_dispatch"]) for attempt in report["attempts"]
            )
            report["repeated_unknown_dispatches"] = sum(
                attempt["repeated_unknown_dispatches"] for attempt in report["attempts"]
            )
            report["unsupported_pass_claims"] = sum(
                attempt["unsupported_pass_claim"] for attempt in report["attempts"]
            )
            (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{report['passing']}/36 passed. Report: {output / 'summary.json'}")
    return 0 if report["passing"] >= 30 and not any(
        report[field] for field in ("out_of_scope_files", "files_changed_without_dispatch",
                                    "repeated_unknown_dispatches", "unsupported_pass_claims")
    ) else 1


if __name__ == "__main__":
    sys.exit(main())
