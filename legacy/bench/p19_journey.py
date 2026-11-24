"""Replay the P19 journeys against the exact executable from a candidate ZIP."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from tempfile import TemporaryDirectory
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests/e2e"))
from supplement_live import HEADING, SUPERSEDED, answer, history, relay, results, send, stage_supplement, tool_reply
from tui_workspace import F2, F3, F8, F10, approve, submit
from two_file_editing import CHECKS, NEW_SOURCE, OLD_SOURCE, OLD_TEST, SOURCE, TEST
from tui import console
from tui_performance import measurements


PROTOCOL = "responses"
EXISTING_TEST = OLD_TEST + CHECKS
FIRST_SOURCE = OLD_SOURCE.replace("number * size", "number * size - 1")
ADDED_TEST = EXISTING_TEST + "# Approval-first fixture retained this second change.\n"
REPAIR = "Fix only the source and rerun the same test."


def version(observed, call):
    return re.search(r"^version=(\S+)$", observed[call], re.M)[1]


def all_tools(snapshot):
    return {tool["id"]: tool for round_ in snapshot["rounds"] for tool in round_["tools"]}


def edit_journey(base, executable, approval_first):
    arrivals = {}
    show_test = "corrected-test"
    failed = "verify-before"
    skipped = "pending-after-failure"
    repaired = "verify-after"
    name = "approval-first" if approval_first else "supplement-first"

    def plan(index, payload):
        items = history(payload, PROTOCOL)
        observed = results(items, PROTOCOL)
        users = [item.get("content") for item in items if item.get("role") == "user"]
        if REPAIR in users:
            if "read-repair" not in observed:
                assert "command exited 1" in observed[failed]
                assert "task closed by user" in observed[skipped]
                calls = [("read-repair", "read", {"path": SOURCE})]
            elif "edit-repair" not in observed:
                calls = [("edit-repair", "edit", {"path": SOURCE, "version": version(observed, "read-repair"),
                          "old_text": "number * size - 1", "new_text": "(number - 1) * size"})]
            elif repaired not in observed:
                assert observed["edit-repair"].startswith("applied ")
                calls = [(repaired, "run", {"program": sys.executable, "args": ["-B", TEST]})]
            else:
                assert "command exited 0" in observed[repaired]
                return send(answer(PROTOCOL, "REPAIR RECORDED"))
            return send(tool_reply(PROTOCOL, calls))
        if index == 0:
            return send(tool_reply(PROTOCOL, [("read-source", "read", {"path": SOURCE}),
                                               ("read-test", "read", {"path": TEST})]))
        if index == 1:
            return send(tool_reply(PROTOCOL, [
                ("edit-source", "edit", {"path": SOURCE, "version": version(observed, "read-source"),
                                         "old_text": OLD_SOURCE, "new_text": FIRST_SOURCE}),
                (show_test, "edit", {"path": TEST, "version": version(observed, "read-test"),
                                     "old_text": EXISTING_TEST, "new_text": ADDED_TEST}),
            ]))
        if approval_first and index == 2:
            assert HEADING not in users and observed[show_test].startswith("applied ")
            return send(answer(PROTOCOL, "OLD DIRECTION"), partial=b"")
        assert users.count(HEADING) == 1
        assert observed["edit-source"].startswith("applied ")
        expected_second = "applied " if approval_first else SUPERSEDED
        assert expected_second in observed[show_test]
        if failed not in observed:
            arrivals["next_model_post"] = time.perf_counter_ns()
            assert items[-1]["content"] == HEADING
            return send(tool_reply(PROTOCOL, [(failed, "run", {"program": sys.executable, "args": ["-B", TEST]})]))
        assert "command exited 1" in observed[failed]
        return send(tool_reply(PROTOCOL, [(skipped, "run", {"program": sys.executable, "args": ["-B", TEST]})]))

    with relay(base, name, executable, executable, PROTOCOL, plan, None) as (fixture, server):
        fixture.args += ["--allow-run", "--write", SOURCE, "--write", TEST]
        for name_, contents in ((SOURCE, OLD_SOURCE), (TEST, EXISTING_TEST)):
            target = fixture.root / name_
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents.encode())
        test_bytes = (ADDED_TEST if approval_first else EXISTING_TEST).encode()

        def screen(*flags):
            return console(executable, ["--tui", *fixture.args, "--mono", *flags], 28, 110, fixture.env)

        with screen() as terminal:
            terminal.expect("等待任务")
            submit(terminal, "Fix the source and update the pagination tests.")
            terminal.expect("任务 1  /  edit-source")
            approve(terminal)
            terminal.expect("任务 1  /  corrected-test")
            assert (fixture.root / SOURCE).read_bytes() == FIRST_SOURCE.encode()
            if approval_first:
                approve(terminal)
                assert server.arrived.wait(5)
            assert (fixture.root / TEST).read_bytes() == test_bytes
            stage_supplement(terminal)
            entered = time.perf_counter_ns()
            terminal.send(F2)
            terminal.expect("任务 1  /  verify-before")
            confirmed = time.perf_counter_ns()
            approve(terminal)
            terminal.expect("任务 1  /  pending-after-failure")
            terminal.key("\x03", "已停止")
            terminal.key(F3, "再次按 F3")
            terminal.key(F3, "F2 提交草稿")
        server.release.set()
        first, phases = fixture.saved()
        tools = all_tools(first)
        first_revision = 2 if approval_first else 1
        assert tools[failed]["verification"]["code"] == 1 and tools[failed]["run"]["edit_revision"] == first_revision
        assert tools[show_test]["dispatched"] == approval_first and tools[skipped]["result"] == "task closed by user"
        assert first["requests"] == len(server.requests) == (5 if approval_first else 4)
        assert first["dispatches"] == (5 if approval_first else 4)
        assert phases.count("supplement") == phases.count("supplement_included") == 1
        line = fixture.command(resume=True, extra=("--inspect",), data=b"", driver=False)
        assert f"command exited 1 at edit revision {first_revision}" in line
        assert ((f"[edit] applied {TEST}" if approval_first else f"[edit] {SUPERSEDED}") in line)

        with screen("--resume") as terminal:
            terminal.expect("已停止")
            terminal.key(F8, "call verify-before")
            terminal.expect(f"command exited 1 at edit revision {first_revision}")
            terminal.send(F10)
            terminal.quiet()
            assert fixture.saved()[0] == first and len(server.requests) == first["requests"]
            submit(terminal, REPAIR)
            terminal.expect("任务 2  /  edit-repair")
            approve(terminal)
            terminal.expect("任务 2  /  verify-after")
            approve(terminal)
            terminal.expect("REPAIR RECORDED")
            terminal.expect("已完成")
        last, _ = fixture.saved()
        verify = all_tools(last)[repaired]
        assert verify["verification"]["code"] == 0 and verify["run"]["edit_revision"] == first_revision + 1
        assert last["requests"] == first["requests"] + 4 == len(server.requests)
        assert last["dispatches"] == first["dispatches"] + 3
        assert (fixture.root / SOURCE).read_bytes() == NEW_SOURCE.encode() and (fixture.root / TEST).read_bytes() == test_bytes
        rendered = fixture.command(resume=True, extra=("--inspect",), data=b"", driver=False)
        assert f"command exited 0 at edit revision {first_revision + 1}" in rendered
        assert fixture.saved()[0] == last and len(server.requests) == last["requests"]
        return {"ordering": name, "tasks": 2, "requests": last["requests"], "dispatches": last["dispatches"],
                "failed_verification_revision": first_revision, "passing_verification_revision": first_revision + 1,
                "old_test_edit_dispatched": approval_first, "extra_posts_while_browsing_or_inspecting": 0,
                "supplement_and_next_review_observed_ms": (confirmed - entered) / 1e6,
                "supplement_next_post_ms": (arrivals["next_model_post"] - entered) / 1e6,
                "source_sha256": hashlib.sha256(NEW_SOURCE.encode()).hexdigest(),
                "test_sha256": hashlib.sha256(test_bytes).hexdigest()}


def read_only_journey(base, executable):
    def plan(index, payload):
        if index == 0:
            assert all(tool["name"] in ("read", "search") for tool in payload["tools"])
            return send(tool_reply(PROTOCOL, [("read-source", "read", {"path": "a.txt"})]))
        observed = results(history(payload, PROTOCOL), PROTOCOL)
        assert index == 1 and "1: a.txt evidence" in observed["read-source"]
        return send(answer(PROTOCOL, "The evidence is at `a.txt:1`."))

    with relay(base, "read-only", executable, executable, PROTOCOL, plan, None) as (fixture, server):
        before = {p.name: p.read_bytes() for p in fixture.root.iterdir()}
        with console(executable, ["--tui", *fixture.args, "--mono"], 24, 80, fixture.env) as terminal:
            terminal.expect("等待任务")
            submit(terminal, "Where is the evidence?")
            terminal.expect("a.txt:1")
            terminal.expect("已完成")
            assert "待批准" not in terminal.text
        assert before == {p.name: p.read_bytes() for p in fixture.root.iterdir()}
        sample, _ = fixture.saved()
        line = fixture.command(resume=True, extra=("--inspect",), data=b"", driver=False)
        assert "`a.txt:1`" in line
        assert "Where is the evidence?" == sample["tasks"][0]["question"]
        evidence = all_tools(sample)["read-source"]
        assert evidence["version"] and evidence["observed_utc"] and "1: a.txt evidence" in evidence["result"]
        assert len(sample["tasks"]) == sample["dispatches"] == 1 and sample["requests"] == len(server.requests) == 2
        return {"tasks": 1, "requests": 2, "read_dispatches": 1, "approval_count": 0, "changed_files": [],
                "citation": "a.txt:1"}


def repeated_failure_journey(base, executable):
    def plan(index, _):
        assert index in (0, 1)
        return send(tool_reply(PROTOCOL, [(f"failed-read-{index}", "read", {"path": "binary.txt"})]))

    with relay(base, "repeated-failure", executable, executable, PROTOCOL, plan, None) as (fixture, server):
        (fixture.root / "binary.txt").write_bytes(b"\x00invalid text")
        with console(executable, ["--tui", *fixture.args, "--mono"], 26, 110, fixture.env) as terminal:
            terminal.expect("等待任务")
            submit(terminal, "Inspect the binary fixture")
            terminal.expect("任务暂停 · repeated_file_failure")
            terminal.expect("Calls failed-read-0 and failed-read-1")
            terminal.key(F3, "再次按 F3")
            terminal.key(F3, "F2 提交草稿")
        snap, _ = fixture.saved()
        assert snap["tasks"][0]["closed"] and snap["requests"] == len(server.requests) == snap["dispatches"] == 2
        line = fixture.command(resume=True, extra=("--inspect",), data=b"", driver=False)
        assert "Calls failed-read-0 and failed-read-1" in line and len(server.requests) == 2
        return {"paused_and_closed": True, "requests": 2, "tool_dispatches": 2, "additional_posts_after_close": 0}


def correction_timings(base, executable, samples):
    painted, saved, sent = [], [], []
    for index in range(samples):
        timepoints = {}

        def plan(attempt, payload):
            if attempt == 0:
                return send(answer(PROTOCOL, "Old direction"), partial=b"")
            assert attempt == 1 and history(payload, PROTOCOL)[-1]["content"] == HEADING
            timepoints["next_post"] = time.perf_counter_ns()
            return send(answer(PROTOCOL, "CORRECTION FINISHED"))

        with relay(base, f"busy-input-{index}", executable, executable, PROTOCOL, plan, None) as (fixture, server):
            with console(executable, ["--tui", *fixture.args, "--mono"], 24, 80, fixture.env) as terminal:
                terminal.expect("等待任务")
                submit(terminal, "Wait for the first response")
                assert server.arrived.wait(5)
                stage_supplement(terminal)

                initial = len(terminal.text)
                entered = time.perf_counter_ns()
                terminal.send(F2)
                acknowledgments = ("补充已保存", "补充已纳入请求")
                markers = ("补充等待保存", *acknowledgments)
                def observed(labels):
                    limit = time.monotonic() + 5
                    while not any(mark in terminal.text[initial:] for mark in labels):
                        if time.monotonic() >= limit:
                            raise AssertionError(f"No confirmation reached ConPTY for {labels}")
                        terminal.collect()
                    return time.perf_counter_ns()

                timepoints["visible"] = observed(markers)
                timepoints["saved"] = observed(acknowledgments)
                terminal.expect("CORRECTION FINISHED")
                terminal.expect("已完成")
                server.release.set()
            snapshot, checkpoints = fixture.saved()
            assert len(server.requests) == 2 and snapshot["dispatches"] == 0 and checkpoints.count("supplement") == 1
            painted.append((timepoints["visible"] - entered) / 1e6)
            saved.append((timepoints["saved"] - entered) / 1e6)
            sent.append((timepoints["next_post"] - entered) / 1e6)
    return {"feedback_seen_by_conpty": measurements(painted), "durable_acknowledgment_seen_by_conpty": measurements(saved),
            "next_post_at_loopback": measurements(sent)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--input-samples", type=int, default=0)
    args = parser.parse_args()
    app = args.executable.resolve(strict=True)
    with TemporaryDirectory(prefix="lay-p19-journey-") as directory:
        base = Path(directory)
        report = {"executable_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
                  "edit_journeys": [edit_journey(base, str(app), first) for first in (False, True)],
                  "read_only": read_only_journey(base, str(app)),
                  "repeated_failure": repeated_failure_journey(base, str(app))}
        if args.input_samples:
            report["busy_input"] = correction_timings(base, str(app), args.input_samples)
        print(json.dumps(report, indent=2))
