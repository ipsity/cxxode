"""Replay three-file changes to frozen Lay Code source with independent build checks."""

import argparse
import hashlib
from io import BytesIO
import json
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/e2e"))
from supplement_live import answer, history, relay, results, send, tool_reply
from tui import console
from tui_workspace import approve, submit


SOURCE = "266f438a6e8f24ba7988da86ce3d64c8d9bddff7"
PROTOCOL = "responses"
CONFIG = "tests/fixtures/empty-config"
OLD_CONDITION = 'NOT settings MATCHES "empty_config = allow"'
NEW_CONDITION = 'NOT (settings MATCHES "accept_empty = true" OR settings MATCHES "empty_config = allow")'
CASES = {
    "cpp-symbol-rename": (
        "Rename edit_progress to describe_file_edits across the declaration, definition and caller.",
        {path: ("edit_progress", "describe_file_edits") for path in
         ("src/tools/edit.hpp", "src/tools/edit.cpp", "app/main.cpp")},
        "src/tools/read_only.hpp",
    ),
    "config-and-ctest": (
        "Use accept_empty = true, keep the old allow setting compatible and rename the CTest case.",
        {f"{CONFIG}/config.txt": ("empty_config = reject", "accept_empty = true"),
         f"{CONFIG}/check.cmake": (OLD_CONDITION, NEW_CONDITION),
         f"{CONFIG}/CMakeLists.txt": ("NAME empty_config", "NAME accept_empty_config")},
        f"{CONFIG}/sample.conf",
    ),
}


def archive():
    return subprocess.check_output(["git", "archive", "--format=zip", SOURCE, "app", "src", CONFIG], cwd=ROOT)


def populate(root, frozen):
    with ZipFile(BytesIO(frozen)) as files:
        files.extractall(root)


def changed(root, patches):
    return {path: (root / path).read_bytes().count(new.encode()) == 1 and
            old.encode() not in (root / path).read_bytes() for path, (old, new) in patches.items()}


def hidden_check(case, root, args, expected):
    def command(*argv):
        return subprocess.run(argv, cwd=root, capture_output=True, timeout=45)

    if case == "cpp-symbol-rename":
        stage = command(args.cxx, "-std=c++20", "-fsyntax-only", "-DLAY_WITH_ANTHROPIC", "-Isrc",
                        "app/main.cpp", "src/tools/edit.cpp")
        if not expected:
            assert b"edit_progress" in stage.stderr, stage.stderr.decode(errors="replace")
        else:
            assert stage.returncode == 0, stage.stderr.decode(errors="replace")
        return stage.returncode == 0

    with TemporaryDirectory(prefix="lay-p21-config-") as build:
        configure = command(args.cmake, "-S", CONFIG, "-B", build)
        assert configure.returncode == 0, configure.stderr.decode(errors="replace")
        check = command(args.ctest, "--test-dir", build, "-C", "Release", "--output-on-failure", "--no-tests=error",
                        "-R", "^accept_empty_config$")
        if not expected:
            assert b"No tests were found" in check.stdout + check.stderr
            return check.returncode == 0
        assert check.returncode == 0 and b"accept_empty_config" in check.stdout, (check.stdout + check.stderr).decode(
            errors="replace")
        for setting, ok in (("empty_config = allow", True), ("accept_empty = false", False)):
            path = root / CONFIG / "hidden-policy.txt"
            path.write_text(setting, encoding="utf-8")
            try:
                compatibility = command(args.cmake, "-DSETTINGS_FILE=hidden-policy.txt", "-P", f"{CONFIG}/check.cmake")
                assert (compatibility.returncode == 0) == ok, compatibility.stderr.decode(errors="replace")
            finally:
                path.unlink()
        return check.returncode == 0


def journey(base, label, case, fixture_bytes, args, previous=None):
    question, patches, neighbor = CASES[case]
    paths = list(patches)
    requested = paths[-1:] if previous else paths
    scope = paths[:2] if args.baseline and not previous else requested

    def plan(index, payload):
        observed = results(history(payload, PROTOCOL), PROTOCOL)
        if index == 0:
            return send(tool_reply(PROTOCOL, [(f"read-{i}", "read", {"path": path})
                                              for i, path in enumerate(requested)]))
        if index == 1:
            versions = [re.search(r"^version=(\S+)$", observed[f"read-{i}"], re.M)[1]
                        for i in range(len(requested))]
            return send(tool_reply(PROTOCOL, [(f"edit-{i}", "edit", {"path": path, "version": version,
                                               "old_text": patches[path][0], "new_text": patches[path][1]})
                                              for i, (path, version) in enumerate(zip(requested, versions))]))
        if index == 2:
            return send(tool_reply(PROTOCOL, [("outside", "edit", {"path": neighbor, "version": "absent",
                                                                    "new_text": "neighbor must stay unchanged"})]))
        assert index == 3 and "outside" in observed
        return send(answer(PROTOCOL, "Review replay finished."))

    app = str(args.executable)
    with relay(base, label, app, app, PROTOCOL, plan, None) as (fixture, server):
        populate(fixture.root, fixture_bytes)
        if previous:
            for path in paths[:2]:
                (fixture.root / path).write_bytes(previous[path])
        originals = {path: (fixture.root / path).read_bytes() for path in paths + [neighbor]}
        if not previous:
            assert all(value.count(patches[path][0].encode()) == 1 for path, value in originals.items() if path in patches)
        fixture.args += [argument for path in scope for argument in ("--write", path)]
        rejected = False
        if args.baseline and not previous:
            rejected = "excessive" in fixture.command(2, driver=False, data=b"",
                                                       extra=("--write", paths[2], "--question", question))
            assert rejected and not server.requests and not fixture.db.exists()

        review_ms = []
        with console(app, ["--tui", *fixture.args, "--mono"], 32, 120, fixture.env) as screen:
            screen.expect("等待任务")
            submit(screen, question)
            for path in scope:
                screen.expect("待批准 · 编辑 " + path)
                started = time.perf_counter_ns()
                screen.expect(patches[path][1][:48])
                assert (fixture.root / path).read_bytes() == originals[path]
                approve(screen)
                review_ms.append(round((time.perf_counter_ns() - started) / 1e6, 3))
            screen.expect("Review replay finished.")
            screen.expect("已完成")
        saved, _ = fixture.saved()
        tools = {tool["id"]: tool for group in saved["rounds"] for tool in group["tools"]}
        edits = [tools[f"edit-{i}"] for i in range(len(requested))]
        assert sum(tool["dispatched"] for tool in edits) == len(scope)
        assert all(tool["edit"]["approved"] and tool["status"] == 1 for tool in edits[:len(scope)])
        assert not tools["outside"]["dispatched"] and "outside" in tools["outside"]["result"]
        assert (fixture.root / neighbor).read_bytes() == originals[neighbor]
        assert saved["requests"] == len(server.requests) == 4
        shown = fixture.command(driver=False, resume=True, data=b"", extra=("--inspect",))
        assert saved == fixture.saved()[0] and len(server.requests) == 4
        if len(scope) > 1:
            assert all(f"[edit progress] {path} [applied]" in shown for path in scope)
        if rejected:
            assert not edits[-1]["dispatched"] and "outside" in edits[-1]["result"]
            fixture.args[-1] = paths[2]
            assert "session scope changed" in fixture.command(2, driver=False, resume=True, data=b"",
                                                               extra=("--inspect",))
            assert saved == fixture.saved()[0] and len(server.requests) == 4
        complete = not rejected
        assert changed(fixture.root, patches) == {path: complete or path in scope for path in paths}
        assert hidden_check(case, fixture.root, args, complete) == complete
        return {"edit_approvals": len(scope), "observed_review_ms_per_file": review_ms, "posts": len(server.requests),
                "three_path_launch_rejected": rejected, "hidden_behavior_passed": complete,
                "adjacent_file_unchanged": True}, {path: (fixture.root / path).read_bytes() for path in paths}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--baseline", action="store_true")
    args = parser.parse_args()
    args.executable = args.executable.resolve(strict=True)
    paths = archive()
    with TemporaryDirectory(prefix="lay-p21-scope-") as directory:
        base = Path(directory)
        report = {"frozen_source": SOURCE, "workspaces_sha256": hashlib.sha256(paths).hexdigest(),
                  "executable_sha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                  "protocol": PROTOCOL, "model": "scripted loopback", "cases": {}}
        for case in CASES:
            single, prior = journey(base, case, case, paths, args)
            case_report = {"single_session": single}
            if args.baseline:
                split, _ = journey(base, case + "-split", case, paths, args, prior)
                case_report["split_two_sessions"] = {
                    "hidden_behavior_passed": split["hidden_behavior_passed"],
                    "posts": single["posts"] + split["posts"],
                    "edit_approvals": single["edit_approvals"] + split["edit_approvals"],
                    "observed_review_ms_per_file": single["observed_review_ms_per_file"] + split["observed_review_ms_per_file"],
                }
            report["cases"][case] = case_report
        report["single_session_success"] = sum(case["single_session"]["hidden_behavior_passed"]
                                               for case in report["cases"].values())
        report["case_count"] = len(report["cases"])
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
