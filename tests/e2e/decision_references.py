"""Check the live CLI/TUI decision reference and its atomic session upgrade."""

from contextlib import closing, contextmanager
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import sys
from tempfile import TemporaryDirectory

from editing import finish, wait_for
from supplement_live import answer, history, relay, send
from tui import ESC
from tui_workspace import submit
from two_file_editing import tui


EARLIER = "Keep the legacy setting for importer compatibility."
CURRENT = "The new importer is live. Withdraw the requirement to keep the legacy setting."
QUESTION = "Which importer decision is current?"
FOLLOW = ("--follow-up", QUESTION, "--no-write", "--quote-task", "1", "--current-decision-task", "2")
HEADER = "[Host selected decision references]\n"
RESPONSE = "Task 2 contains the current decision."
F11, F12 = ESC + "[23~", ESC + "[24~"
PROTOCOL = "responses"


def source(task, text):
    return dict(task=task, message=f"task-{task}-question", version=hashlib.sha256(text.encode()).hexdigest())


SOURCES = {"earlier": source(1, EARLIER), "current": source(2, CURRENT)}


@contextmanager
def example(base, label, cli, driver, legacy):
    def plan(index, payload):
        items = history(payload, PROTOCOL)
        if index < 2:
            assert items[-1]["content"] == (EARLIER, CURRENT)[index]
            return send(answer(PROTOCOL, f"Model observation {index + 1}."))
        assert items[-1]["content"] == QUESTION
        message = items[-2]["content"]
        assert message.startswith(HEADER)
        refs = json.loads(message.removeprefix(HEADER))
        assert refs == {"earlier": {**SOURCES["earlier"], "role": "user", "text": EARLIER},
                        "current": {**SOURCES["current"], "role": "user", "text": CURRENT}}
        assert set(tool["name"] for tool in payload["tools"]) == {"read", "search"}
        return send(answer(PROTOCOL, RESPONSE))

    with relay(base, label, cli, driver, PROTOCOL, plan, None) as (fixture, server):
        if legacy:
            fixture.cli = legacy
        fixture.command(driver=False, data=b"", extra=("--question", EARLIER))
        fixture.command(driver=False, data=b"", extra=("--follow-up", CURRENT, "--no-write"))
        fixture.cli = cli
        assert format_of(fixture) == 4 and len(server.requests) == 2
        yield fixture, server


def format_of(fixture):
    with closing(sqlite3.connect(fixture.db)) as db:
        return db.execute("PRAGMA user_version").fetchone()[0]


def assert_saved(fixture):
    snapshot, _ = fixture.saved()
    task = snapshot["tasks"][-1]
    assert task["question"] == QUESTION and task["decision_reference"] == SOURCES and snapshot["dispatches"] == 0
    assert format_of(fixture) == 5
    return snapshot


def terminal_selection(base, cli, driver, legacy):
    with example(base, "decision-selection", cli, driver, legacy) as (fixture, server):
        with tui(fixture, "--resume") as screen:
            screen.expect("已完成")
            screen.key(ESC + "[H", "> #1")
            screen.key(ESC + "[B", "> 助手  Model observation 1.")
            screen.key(F11, "请先选中一条已保存的用户原文")
            screen.key(ESC + "[A", "> #1")
            screen.key(F11, "引用任务 1")
            screen.key(F11, "已移除草稿引用")
            screen.key(F11, "引用任务 1")
            screen.key(ESC + "[B", "> 助手  Model observation 1.")
            screen.key(ESC + "[B", "> #2")
            screen.key(F12, "引用任务 1 · 现行任务 2")
            assert len(server.requests) == 2 and format_of(fixture) == 4
            submit(screen, QUESTION)
            screen.expect(RESPONSE)
            screen.expect("已完成")
        snapshot = assert_saved(fixture)
        assert len(server.requests) == 3
        output = fixture.command(driver=False, resume=True, data=b"", extra=("--inspect",))
        assert "[decision reference] task-1-question" in output and "[current decision reference] task-2-question" in output
        assert fixture.saved()[0] == snapshot
        fixture.command(driver=False, data=b"", extra=FOLLOW[:-2])
        snapshot = assert_saved(fixture)
        assert len(server.requests) == 4
        if legacy:
            fixture.cli = legacy
            assert "unsupported session database format" in fixture.command(2, driver=False, resume=True,
                                                                            extra=("--inspect",), data=b"")
            assert fixture.saved()[0] == snapshot and len(server.requests) == 4


def durable_reference(base, cli, driver, legacy):
    with example(base, "decision-atomic", cli, driver, legacy) as (fixture, server):
        previous, _ = fixture.saved()
        wrong = FOLLOW[:-1] + ("3",)
        assert "must name saved tasks" in fixture.command(2, driver=False, data=b"", extra=wrong)
        assert "must name a newer task" in fixture.command(2, driver=False, data=b"", extra=FOLLOW[:-1] + ("1",))
        assert "require --follow-up" in fixture.command(2, driver=False, resume=True, data=b"",
                                                         extra=("--quote-task", "1"))
        assert "positive task IDs" in fixture.command(2, driver=False, data=b"", extra=FOLLOW[:-1] + ("-1",))
        for point in ("before_follow_up", "after_follow_up"):
            process = fixture.paused(point, data=b"", extra=FOLLOW)
            try:
                wait_for(process, f"[checkpoint] {point}".encode())
                if point == "before_follow_up":
                    assert format_of(fixture) == 4 and fixture.saved()[0] == previous
                else:
                    current = assert_saved(fixture)
                    assert current["requests"] == previous["requests"] and not current["finished"]
                assert len(server.requests) == 2
            finally:
                finish(process, kill=True)
        assert RESPONSE in fixture.command(driver=False, resume=True, data=b"")
        assert len(server.requests) == 3 and assert_saved(fixture)["finished"]


if __name__ == "__main__":
    if os.name == "nt":
        from tui import PtyProcess
        if PtyProcess is None:
            sys.exit(77)
    with TemporaryDirectory(prefix="lay-decision-") as directory:
        cli, driver, *previous = sys.argv[1:]
        legacy = previous[0] if previous else None
        terminal_selection(Path(directory), cli, driver, legacy)
        durable_reference(Path(directory), cli, driver, legacy)
    print("Verified selected sources, saved corrections and the atomic decision format transition.")
