"""Run the frozen live quality catalog on Windows with bounded fixture approvals."""

import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import time

from winpty import Backend, PtyProcess

from live_quality import CATALOG, context, run_case


def pending_action(database, tool):
    for round_ in reversed(context(database).get("rounds", [])):
        for record in round_["tools"]:
            if record["name"] == tool and record["status"] == 0 and not record["dispatched"]:
                return {key: value for key, _, value in record["arguments"]}
    return {}


def approve_edit(case, root, request):
    if request.get("path") not in case["allow"] or "reference" not in case:
        return False
    path = root / request["path"]
    original = (case["files"] | case.get("dirty", {}))[request["path"]]
    source = path.read_text(encoding="utf-8")
    old, new = request.get("old_text", ""), request.get("new_text", "")
    if source != original or not old or source.count(old) != 1:
        return False
    expected = original.replace(*case["reference"], 1)
    reviewed = [expected]
    if case["id"] == "fix-empty-config":
        reviewed.append("def parse(text):\n    text = text.strip()\n    if not text:\n        return {}\n"
                        "    key, value = text.split('=', 1)\n    return {key: value}\n")
    try:
        proposed = ast.dump(ast.parse(source.replace(old, new, 1)))
        return any(proposed == ast.dump(ast.parse(variant)) for variant in reviewed)
    except (SyntaxError, ValueError):
        return False


def approve_run(case, request):
    if case.get("verification", {}).get("status") != "target_missing":
        return False
    if request.get("path", ".") not in ("", "."):
        return False
    name = request.get("program", "")
    resolved = shutil.which(name) if name else None
    if not resolved or Path(resolved).name.lower() not in ("python.exe", "python3.exe"):
        return False
    trusted = {Path(sys.executable).resolve(), *(Path(path).resolve()
               for executable in ("python.exe", "python3.exe") if (path := shutil.which(executable)))}
    if Path(resolved).resolve() not in trusted:
        return False
    arguments = request.get("args", [])
    return (bool(arguments) and all(arg in ("-B", "-I", "-u") for arg in arguments[:-1]) and
            arguments[-1].replace("\\", "/") in ("tests/smoke.py", "./tests/smoke.py"))


def controlled_terminal(decisions):
    def run(command, env, case, root, database):
        prompts = {"Apply this edit? [y/N] ": "edit", "Run this command? [y/N] ": "run"}
        process = PtyProcess.spawn(list(map(str, command)), env=env, dimensions=(34, 300), backend=Backend.ConPTY)
        tail = ""
        deadline = time.monotonic() + 240
        try:
            while process.isalive():
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"Live fixture {case['id']} timed out")
                if not select.select([process.fileobj], [], [], 0.10)[0]:
                    continue
                try:
                    tail += process.read(32768)
                except EOFError:
                    break
                for prompt, tool in prompts.items():
                    if prompt not in tail:
                        continue
                    _, _, tail = tail.partition(prompt)
                    request = pending_action(database, tool)
                    granted = approve_edit(case, root, request) if tool == "edit" else approve_run(case, request)
                    decisions.append({"action": tool, "approved": granted})
                    process.write("y\r" if granted else "\r")
                tail = tail[-4096:]
            while process.isalive() and time.monotonic() < deadline:
                time.sleep(0.02)
            if process.exitstatus is None:
                raise RuntimeError(f"Missing exit status for {case['id']}")
            return process.exitstatus
        finally:
            process.close(force=True)
    return run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--provider-config", type=Path, required=True)
    parser.add_argument("--provider", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.cli = args.cli.resolve(strict=True)
    args.provider_config = args.provider_config.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir()
    config = json.loads(args.provider_config.read_text(encoding="utf-8-sig"))
    profile = config["providers"][args.provider]
    cases = json.loads(CATALOG.read_text(encoding="utf-8"))
    assert len(cases) == 12
    report = {
        "model": args.model, "provider": args.provider, "protocol": profile["protocol"],
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "executable_sha256": hashlib.sha256(args.cli.read_bytes()).hexdigest(),
        "catalog_sha256": hashlib.sha256(CATALOG.read_bytes()).hexdigest(),
        "approval_policy": "Approve reference-equivalent Python AST edits, the reviewed strip-once variant, and the exact missing-script Python invocation.",
        "attempts": [],
    }
    for repeat in (1, 2, 3):
        for case in cases:
            decisions = []
            attempt = run_case(args, case, repeat, output, controlled_terminal(decisions))
            attempt["approval_decisions"] = decisions
            report["attempts"].append(attempt)
            report["passing"] = sum(row["passed"] for row in report["attempts"])
            (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(f"RESULT {case['id']} {repeat} {attempt['passed']}", flush=True)
    return int(report["passing"] < 30 or any(
        row["out_of_scope"] or row["files_changed_without_dispatch"] or row["repeated_unknown_dispatches"] or
        row["unsupported_pass_claim"] for row in report["attempts"]))


if __name__ == "__main__":
    sys.exit(main())
