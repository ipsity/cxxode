"""Verify v3 migration and durable same-task supplement checkpoints."""

from contextlib import closing
from pathlib import Path
import sqlite3
import subprocess
import sys
import os
from tempfile import TemporaryDirectory

from editing import Fixture, finish, wait_for
from two_file_editing import NEW_SOURCE, OLD_TEST, QUESTION, approvals, changes, pager, tui


SUPPLEMENT = "保留刚才的分页修改，先用已有测试验证。"
INPUT_ID = "task-1-input-2"


def db_state(path):
    with closing(sqlite3.connect(path)) as db:
        return (db.execute("PRAGMA user_version").fetchone()[0],
                db.execute("SELECT scope, revision, snapshot FROM session").fetchone())


def migrate_v3(fixture):
    # With no supplement, the snapshot has the original v3 shape.
    before, phases = fixture.saved()
    assert all("supplement" not in task for task in before["tasks"])
    with closing(sqlite3.connect(fixture.db)) as db:
        db.execute("PRAGMA user_version=3")
    _, original = db_state(fixture.db)
    assert "--migrate-session" in fixture.command(2, driver=False, resume=True, data=b"")
    identifier = fixture.args[fixture.args.index("--session") + 1]
    invoke = [fixture.cli, "--migrate-session", identifier]
    migrated = subprocess.run(invoke, env=fixture.env, capture_output=True, timeout=10, check=True)
    assert b"Migrated session to format 4" in migrated.stdout
    backup, = fixture.db.parent.glob(f"session-{identifier}.v3-*.sqlite3")
    assert db_state(backup) == (3, original)
    version, current = db_state(fixture.db)
    assert version == 4 and current[:2] == (original[0], original[1] + 1)
    assert fixture.saved() == (before, phases + ["migrated_v3_to_v4"])
    subprocess.run(invoke, env=fixture.env, capture_output=True, timeout=10, check=True)
    assert db_state(fixture.db) == (version, current)


def kill_at(fixture, point, *, data=b"", extra=(), inspect=None):
    process = fixture.paused(point, data=data, extra=extra)
    try:
        wait_for(process, f"[checkpoint] {point}".encode())
        if inspect:
            inspect()
    finally:
        finish(process, kill=True)


def first_edit(fixture, server):
    kill_at(fixture, "after_edit_result", data=(QUESTION + "\ny\n").encode())
    assert changes(fixture) == [NEW_SOURCE, OLD_TEST]
    assert server.requests == 2


def run(base, cli, driver):
    complete = Fixture(base, "v3-completed", cli, driver)
    complete.command()
    migrate_v3(complete)
    args = ("--test-supplement", SUPPLEMENT)
    assert "cannot accept a supplement" in complete.command(2, resume=True, extra=args, data=b"")

    with pager(base, "v3-dispatched", cli, driver) as (fixture, server):
        first_edit(fixture, server)
        kill_at(fixture, "after_edit_dispatch", data=b"y\n", extra=("--resume",))
        migrate_v3(fixture)
        checkpoint = fixture.saved()
        a, b = approvals(checkpoint[0])
        assert a["status"] == 1 and b["status"] == 0 and b["dispatched"]
        assert "cannot accept a supplement" in fixture.command(2, resume=True, extra=args, data=b"")
        assert fixture.saved() == checkpoint
        assert "effect unknown" in fixture.command(1, driver=False, resume=True, data=b"")
        recovered = fixture.saved()[0]
        assert recovered["dispatches"] == checkpoint[0]["dispatches"]
        assert recovered["requests"] == server.requests == 2
        assert changes(fixture) == [NEW_SOURCE, OLD_TEST]

    with pager(base, "v3-review", cli, driver) as (fixture, server):
        first_edit(fixture, server)
        assert "approval_required" in fixture.command(1, driver=False, resume=True, data=b"")
        migrate_v3(fixture)
        before, phases = fixture.saved()
        assert not approvals(before)[1]["dispatched"]
        revision = db_state(fixture.db)[1][1]

        def unchanged():
            assert fixture.saved() == (before, phases)
            assert changes(fixture) == [NEW_SOURCE, OLD_TEST] and server.requests == 2

        kill_at(fixture, "before_supplement", extra=("--resume", *args), inspect=unchanged)
        unchanged()
        failed = fixture.command(2, resume=True, extra=args, data=b"", LAY_TEST_FULL="before_supplement")
        assert "disk is full" in failed and INPUT_ID not in failed
        unchanged()

        def accepted():
            saved, checkpoints = fixture.saved()
            assert checkpoints == phases + ["supplement"]
            assert saved["requests"] == before["requests"] and saved["dispatches"] == before["dispatches"]
            assert saved["rounds"] == before["rounds"]
            assert saved["tasks"][0]["first_request"] == before["tasks"][0]["first_request"]
            assert saved["tasks"][0]["supplement"] == {
                "id": INPUT_ID, "task_id": 1, "sequence": 2, "text": SUPPLEMENT,
                "saved_revision": revision + 1, "before_round": None,
            }
            assert changes(fixture) == [NEW_SOURCE, OLD_TEST]
            return saved, checkpoints

        kill_at(fixture, "after_supplement", extra=("--resume", *args), inspect=accepted)
        accepted()
        kill_at(fixture, "before_supplement_resume", extra=("--resume",), inspect=accepted)
        accepted()
        reopened = fixture.command(driver=False, resume=True, extra=("--inspect",), data=b"")
        expected = f"[supplement {INPUT_ID}] saved at revision {revision + 1} / pending"
        assert reopened.count(expected) == 1 and SUPPLEMENT in reopened
        assert "Apply this edit?" not in reopened and server.requests == 2
        accepted()
        assert expected in fixture.command(resume=True, extra=args, data=b"")
        assert "already has a saved supplement" in fixture.command(
            2, resume=True, extra=("--test-supplement", "另一个补充"), data=b"")
        accepted()

        with tui(fixture, "--resume") as terminal:
            terminal.expect("任务暂停 · 补充待处理")
            terminal.expect("补充已保存 · 等待处理 " + INPUT_ID)
            terminal.send("\x1b[A")
            terminal.quiet()
            terminal.key("\x0f", SUPPLEMENT)
            terminal.key("\x1bOQ", "补充等待处理 · F5 继续原任务")
            accepted()
        closed = fixture.command(driver=False, resume=True, extra=("--close-task",), data=b"")
        assert "task closed before inclusion" in closed and server.requests == 2
        staged, _ = fixture.saved()
        assert staged["tasks"][0]["closed"] and staged["tasks"][0]["supplement"]["before_round"] is None
        assert approvals(staged)[1]["result"] == "task closed by user"
        fixture.command(driver=False, extra=("--follow-up", "换一个问题", "--no-write"), data=b"")
        after, _ = fixture.saved()
        assert server.requests == 3 and after["tasks"][1]["first_request"] == staged["requests"]
        assert "supplement" not in after["tasks"][1]
        assert after["tasks"][0]["supplement"] == staged["tasks"][0]["supplement"]
        assert changes(fixture) == [NEW_SOURCE, OLD_TEST]
        assert after["dispatches"] == before["dispatches"]


if __name__ == "__main__":
    if os.name == "nt":
        from tui import PtyProcess
        if PtyProcess is None:
            sys.exit(77)
    with TemporaryDirectory(prefix="lay-supplement-") as temp:
        run(Path(temp), *sys.argv[1:])
    print("Verified three v3 migration states, supplement crash boundaries, and zero stale action dispatch.")
