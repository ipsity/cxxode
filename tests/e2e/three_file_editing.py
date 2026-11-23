"""Exercise the exact three-file scope across independent approvals and recovery."""

from contextlib import contextmanager
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
from tempfile import TemporaryDirectory

from editing import finish, wait_for
from supplement_live import answer, history, relay, results, send, tool_reply
from tui_workspace import F5, F9, approve
from two_file_editing import approvals, tui


FIRST, SECOND, THIRD = "a.txt", "nested/b.txt", "c.txt"
PATHS = (FIRST, SECOND, THIRD)
CHANGED = b"revised evidence\n"
QUESTION = "Apply the three changes."
REVIEW = b"Apply this edit? [y/N] "
PROTOCOL = "responses"


@contextmanager
def example(base, name, cli, driver, *, create=False):
    def plan(index, payload):
        observed = results(history(payload, PROTOCOL), PROTOCOL)
        if index == 0:
            reads = PATHS[:2] if create else PATHS
            return send(tool_reply(PROTOCOL, [(f"read-{i}", "read", {"path": path}) for i, path in enumerate(reads)]))
        if index == 1:
            calls = []
            for i, path in enumerate(PATHS):
                if create and path == THIRD:
                    arguments = {"path": path, "version": "absent", "new_text": CHANGED.decode()}
                else:
                    version = re.search(r"^version=(\S+)$", observed[f"read-{i}"], re.M)[1]
                    requested = path.upper() if create and path == FIRST and os.name == "nt" else path
                    arguments = {"path": requested, "version": version, "old_text": "original evidence\n",
                                 "new_text": CHANGED.decode()}
                calls.append((f"edit-{i}", "edit", arguments))
            return send(tool_reply(PROTOCOL, calls))
        assert index == 2 and all(f"edit-{i}" in observed for i in range(3))
        return send(answer(PROTOCOL, "Scope fixture finished."))

    with relay(base, name, cli, driver, PROTOCOL, plan, None) as (fixture, server):
        (fixture.root / "nested").mkdir()
        for path in (PATHS[:2] if create else PATHS):
            (fixture.root / path).write_bytes(b"original evidence\n")
        fixture.args += [argument for path in PATHS for argument in ("--write", path)]
        yield fixture, server


def compare_entries(fixture, server, *outcomes):
    posts = len(server.requests)
    states = [f"[edit progress] {path} [{outcome}]" for path, outcome in zip(PATHS, outcomes)]
    line = fixture.command(driver=False, resume=True, extra=("--inspect",), data=b"")
    assert all(state in line for state in states), line
    saved, _ = fixture.saved()
    with tui(fixture, "--resume") as screen:
        for state in states:
            screen.expect(state)
        screen.text = ""
        screen.key(F9, "本任务文件改动")
        if "已授权路径" not in screen.text:
            screen.send("\x1b[6~")
        screen.expect("已授权路径")
        for state in states:
            screen.expect(state)
    assert len(server.requests) == posts and fixture.saved()[0] == saved
    return approvals(saved)


def exact_scope(base, cli, driver):
    with example(base, "three-path-create", cli, driver, create=True) as (fixture, server):
        for alias, diagnostic in ((FIRST, "must be distinct"), ("nested/../c.txt", "workspace-relative file"),
                                  ("nested/*.txt", "workspace-relative file"), ("./c.txt", "workspace-relative file"),
                                  ("nested\\b.txt", "workspace-relative file")):
            fixture.args[-1] = alias
            assert diagnostic in fixture.command(2, driver=False, data=b"", extra=("--question", QUESTION))
        if os.name == "nt":
            fixture.args[-1] = FIRST.upper()
            assert "must be distinct" in fixture.command(2, driver=False, data=b"", extra=("--question", QUESTION))
        fixture.args[-1] = THIRD
        assert not fixture.db.exists() and not server.requests
        assert "approval_required" in fixture.command(1, driver=False, data=b"", extra=("--question", QUESTION))
        pending, _ = fixture.saved()
        assert not any(tool["dispatched"] for tool in approvals(pending))
        with tui(fixture, "--resume") as screen:
            screen.expect("F5 继续")
            screen.send(F5)
            for path in PATHS:
                screen.expect("待批准 · 编辑 " + path)
                if path == THIRD:
                    assert not (fixture.root / path).exists()
                approve(screen)
            screen.expect("Scope fixture finished.")
            screen.expect("已完成")
        assert all((fixture.root / path).read_bytes() == CHANGED for path in PATHS)
        assert len(server.requests) == 3
        records = compare_entries(fixture, server, "applied", "applied", "applied")
        assert all(tool["dispatched"] and tool["edit"]["approved"] for tool in records)
        assert [tool["edit"]["path"] for tool in records] == list(PATHS)
        assert records[2]["edit"]["before"] == "absent"


def second_file_changes(base, cli, driver):
    for fault in ("deny", "conflict", "missing", "permissions"):
        with example(base, "three-path-" + fault, cli, driver) as (fixture, server):
            target = fixture.root / SECOND
            original = target.read_bytes()
            process = fixture.paused("unused", data=(QUESTION + "\n").encode())
            try:
                wait_for(process, REVIEW)
                process.stdin.write(b"y\n")
                process.stdin.flush()
                wait_for(process, REVIEW)
                assert (fixture.root / FIRST).read_bytes() == CHANGED and target.read_bytes() == original
                if fault == "conflict":
                    target.write_bytes(b"different bytes from outside the session\n")
                elif fault == "missing":
                    target.unlink()
                elif fault == "permissions":
                    target.chmod(stat.S_IREAD)
                code, output = finish(process, b"n\n" if fault == "deny" else b"y\n")
                expected = "edit denied by user" if fault == "deny" else "file changed during approval"
                assert code == 0 and expected in output, output
                if fault == "missing":
                    assert not target.exists()
                else:
                    assert target.read_bytes() != CHANGED
            finally:
                if process.poll() is None:
                    finish(process, kill=True)
                if fault == "permissions" and target.exists():
                    target.chmod(stat.S_IREAD | stat.S_IWRITE)
            first, second, third = compare_entries(fixture, server, "applied", "not started", "not started")
            assert first["dispatched"] and not second["dispatched"] and not third["dispatched"]
            assert second["edit"]["approved"] == (fault != "deny")
            assert (fixture.root / THIRD).read_bytes() == original


def reparse_between_edits(base, cli, driver):
    with example(base, "three-path-reparse", cli, driver) as (fixture, server):
        second_parent = (fixture.root / SECOND).parent
        backup = fixture.root / "original-nested"
        adjacent = fixture.root / "adjacent"
        adjacent.mkdir()
        original = (fixture.root / SECOND).read_bytes()
        (adjacent / "b.txt").write_bytes(original)
        process = fixture.paused("after_edit_result", data=(QUESTION + "\ny\n").encode())
        try:
            wait_for(process, b"[checkpoint] after_edit_result")
            second_parent.rename(backup)
            if os.name == "nt":
                subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(second_parent), str(adjacent)],
                               check=True, capture_output=True)
            else:
                second_parent.symlink_to(adjacent, target_is_directory=True)
        finally:
            finish(process, kill=True)
        output = fixture.command(driver=False, resume=True, data=b"")
        assert "edit parent directory is missing or unsupported" in output
        first, second, third = compare_entries(fixture, server, "applied", "not started", "not started")
        assert first["dispatched"] and not second["dispatched"] and not third["dispatched"]
        assert (adjacent / "b.txt").read_bytes() == (backup / "b.txt").read_bytes() == original


def interrupted(base, cli, driver):
    with example(base, "three-path-crash", cli, driver) as (fixture, server):
        for point, data, extra in (("after_edit_result", (QUESTION + "\ny\n").encode(), ()),
                                  ("before_edit_result", b"y\n", ("--resume",))):
            process = fixture.paused(point, data=data, extra=extra)
            try:
                wait_for(process, f"[checkpoint] {point}".encode())
                first, second, third = approvals(fixture.saved()[0])
                assert first["status"] == 1 and first["dispatched"] and not third["dispatched"]
                assert second["status"] == 0 and second["dispatched"] == bool(extra)
                assert len(server.requests) == 2
            finally:
                finish(process, kill=True)
        assert (fixture.root / FIRST).read_bytes() == (fixture.root / SECOND).read_bytes() == CHANGED
        assert (fixture.root / THIRD).read_bytes() != CHANGED
        before = (fixture.root / SECOND).stat()
        first, second, third = compare_entries(fixture, server, "applied", "effect unknown", "not started")
        assert first["status"] == 1 and second["status"] == 4 and not third["dispatched"]
        assert len(server.requests) == 2
        after = (fixture.root / SECOND).stat()
        assert (before.st_ino, before.st_mtime_ns) == (after.st_ino, after.st_mtime_ns)
        assert fixture.saved()[1].count("dispatch") == 5


if __name__ == "__main__":
    if os.name == "nt":
        from tui import PtyProcess
        if PtyProcess is None:
            sys.exit(77)
    with TemporaryDirectory(prefix="lay-three-files-") as directory:
        for check in (exact_scope, second_file_changes, reparse_between_edits, interrupted):
            check(Path(directory), *sys.argv[1:])
    print("Verified exact scope, creation, path aliases, permission changes and partial recovery.")
