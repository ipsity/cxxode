from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import stat
import subprocess
import sys
from tempfile import TemporaryDirectory


OLD = b"\xef\xbb\xbf# Keep this comment\r\nempty_config = reject\r\nuntouched = 1\r\n"
NEW = OLD.replace(b"reject", b"allow")
FILE = "配置 notes.txt"


class Fixture:
    def __init__(self, base, name, cli, driver, *, data=OLD):
        self.root = base / name / "工作区"
        self.root.mkdir(parents=True)
        self.file = self.root / FILE
        if data is not None:
            self.file.write_bytes(data)
            if os.name != "nt":
                self.file.chmod(0o640)
        self.cli = cli
        self.driver = driver
        self.args = ["--workspace", str(self.root), "--model", "fixture-edit",
                     "--session", name, "--write", FILE]
        private = base / "private"
        self.env = os.environ.copy()
        if os.name == "nt":
            self.env["LOCALAPPDATA"] = str(private)
            state = private / "LayCode"
        else:
            self.env["XDG_STATE_HOME"] = str(private)
            state = private / "lay-code"
        self.db = state / "sessions" / f"session-{name}.sqlite3"
        self.env.pop("LAY_TEST_INTERACTIVE", None)
        self.env.pop("LAY_TEST_FULL", None)
        self.env.pop("LAY_TEST_PAUSE", None)

    def command(self, code=0, *, resume=False, data=b"fix fixture\ny\n", driver=True, extra=(), **env):
        program = self.driver if driver else self.cli
        settings = {**self.env, **({"LAY_TEST_INTERACTIVE": "1"} if driver else {}), **env}
        cli = [program, *self.args, *(["--resume"] if resume else []), *extra]
        result = subprocess.run(cli, input=data, capture_output=True, timeout=12, env=settings)
        output = (result.stdout + result.stderr).decode("utf-8").replace("\r\n", "\n")
        assert result.returncode == code, (cli, result.returncode, output)
        return output

    def paused(self, marker, *, data=b"fix fixture\ny\n", extra=()):
        env = {**self.env, "LAY_TEST_INTERACTIVE": "1", "LAY_TEST_PAUSE": marker}
        process = subprocess.Popen([self.driver, *self.args, *extra], env=env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        process.stdin.write(data)
        process.stdin.flush()
        return process

    def saved(self):
        with closing(sqlite3.connect(self.db)) as db:
            snapshot = json.loads(db.execute("SELECT snapshot FROM session").fetchone()[0])
            checkpoints = [phase for phase, in db.execute("SELECT phase FROM checkpoints ORDER BY revision")]
        return snapshot, checkpoints

    def artifacts(self):
        return sorted(path.name for path in self.root.glob(".lay-code-*"))


def wait_for(process, marker):
    def read_until():
        output = bytearray()
        while not output.endswith(marker):
            char = process.stdout.read(1)
            if not char:
                break
            output.extend(char)
        return bytes(output)

    with ThreadPoolExecutor(max_workers=1) as pool:
        pending = pool.submit(read_until)
        try:
            output = pending.result(timeout=10)
        except TimeoutError:
            process.kill()
            raise
    assert output.endswith(marker), output.decode("utf-8", errors="replace")
    return output


def finish(process, data=None, kill=False):
    if kill and process.poll() is None:
        process.kill()
    if data is not None:
        process.stdin.write(data)
        process.stdin.flush()
    process.stdin.close()
    result = process.wait(timeout=10)
    output = process.stdout.read().decode("utf-8", errors="replace")
    process.stdout.close()
    return result, output


def edit_record(snapshot):
    return next((tool for round in snapshot["rounds"] for tool in round["tools"] if tool["name"] == "edit"), None)


def cases(base, cli, driver):
    def new(name, **kwargs):
        return Fixture(base, name, cli, driver, **kwargs)

    regular = new("normal")
    output = regular.command()
    assert "-empty_config = reject\n+empty_config = allow\n" in output, output
    assert f"applied {FILE}" in output and regular.file.read_bytes() == NEW and not regular.artifacts()
    if os.name != "nt":
        assert stat.S_IMODE(regular.file.stat().st_mode) == 0o640
    snapshot, phases = regular.saved()
    assert snapshot["finished"] and edit_record(snapshot)["dispatched"]
    assert phases.count("dispatch") == 2 and phases[-1] == "reply"
    previous = regular.file.stat().st_ino
    assert "Apply this edit?" not in regular.command(resume=True, data=b"", driver=False)
    assert regular.file.stat().st_ino == previous and regular.file.read_bytes() == NEW

    appended = new("saved-follow-up")
    appended.command()
    prior, _ = appended.saved()
    follow_up = ("--follow-up", "重新读取修改后的配置", "--no-write")
    for marker in ("after_follow_up", "after_reply"):
        process = appended.paused(marker, data=b"", extra=follow_up)
        try:
            wait_for(process, f"[checkpoint] {marker}".encode())
            assert "active writer" in appended.command(2, extra=follow_up, data=b"")
            saved, _ = appended.saved()
            assert saved["tasks"][-1]["question"] == follow_up[1]
            if marker == "after_follow_up":
                assert saved["requests"] == prior["requests"] and saved["dispatches"] == prior["dispatches"]
                assert not saved["finished"]
            else:
                assert saved["finished"]
        finally:
            finish(process, kill=True)
        if marker == "after_follow_up":
            assert "not closed" in appended.command(2, extra=follow_up, data=b"")
        shown = appended.command(resume=True, driver=False, data=b"")
        assert follow_up[1] in shown
        current, phases = appended.saved()
        assert current["requests"] == prior["requests"] + 2 and current["dispatches"] == prior["dispatches"] + 1
        assert current["tasks"][-1]["id"] == len(current["tasks"])
        assert phases.count("follow_up") == len(current["tasks"]) - 1
        prior = current

    restricted = new("restricted-follow-up")
    restricted.command()
    output = restricted.command(extra=("--follow-up", "create fixture", "--no-write"), data=b"")
    snapshot, _ = restricted.saved()
    first = snapshot["tasks"][-1]["first_round"]
    edit, = snapshot["rounds"][first]["tools"]
    assert edit["name"] == "edit" and not edit["dispatched"] and "Apply this edit?" not in output
    assert snapshot["dispatches"] == 2 and restricted.file.read_bytes() == NEW

    timed = new("timed-follow-up")
    timed.command()
    process = timed.paused("after_follow_up", data=b"", extra=follow_up)
    try:
        wait_for(process, b"[checkpoint] after_follow_up")
    finally:
        finish(process, kill=True)
    snapshot, _ = timed.saved()
    snapshot["tasks"][-1]["active_ms"] = 15 * 60 * 1000
    with closing(sqlite3.connect(timed.db)) as db:
        db.execute("UPDATE session SET snapshot=?", (json.dumps(snapshot),))
        db.commit()
    assert "time_limit" in timed.command(1, resume=True, driver=False, data=b"")
    expired, _ = timed.saved()
    assert expired["requests"] == snapshot["requests"] and expired["dispatches"] == snapshot["dispatches"]
    assert expired["tasks"][-1]["closed"] and expired["tasks"][-1]["active_ms"] == 15 * 60 * 1000
    timed.command(extra=follow_up, driver=False, data=b"")
    assert timed.saved()[0]["requests"] == expired["requests"] + 2

    denied = new("denied")
    assert "edit denied by user" in denied.command(data=b"fix fixture\nn\n")
    assert denied.file.read_bytes() == OLD and not edit_record(denied.saved()[0])["dispatched"]

    piped = new("piped")
    assert "approval_required" in piped.command(1, driver=False)
    assert piped.file.read_bytes() == OLD
    assert not edit_record(piped.saved()[0])["dispatched"]
    assert f"applied {FILE}" in piped.command(resume=True, data=b"y\n")
    assert piped.file.read_bytes() == NEW

    existing = new("exists")
    assert "target already exists" in existing.command(data=b"create fixture\ny\n")
    assert existing.file.read_bytes() == OLD
    created = new("created", data=None)
    assert f"applied {FILE}" in created.command(data=b"create fixture\ny\n")
    assert created.file.read_bytes() == b"empty_config = allow\n"
    assert not created.artifacts()
    ambiguous = new("ambiguous", data=OLD + b"empty_config = reject\r\n")
    assert "old_text is ambiguous" in ambiguous.command()
    assert ambiguous.file.read_bytes() == OLD + b"empty_config = reject\r\n"
    mixed = new("mixed", data=OLD.replace(b"untouched = 1\r\n", b"untouched = 1\n"))
    assert "mixed newlines" in mixed.command()

    stale = new("stale")
    process = stale.paused("unused", data=b"fix fixture\n")
    try:
        wait_for(process, b"Apply this edit? [y/N] ")
        external = OLD + b"editor wrote this\r\n"
        stale.file.write_bytes(external)
        code, output = finish(process, b"y\n")
        assert code == 0 and "file changed during approval" in output, output
        assert stale.file.read_bytes() == external and not edit_record(stale.saved()[0])["dispatched"]
    finally:
        if process.poll() is None:
            finish(process, kill=True)

    for point in ("before_edit_reply", "after_edit_reply", "before_edit_dispatch"):
        fixture = new(point)
        process = fixture.paused(point, data=b"fix fixture\ny\n" if point == "before_edit_dispatch" else b"fix fixture\n")
        try:
            wait_for(process, f"[checkpoint] {point}".encode())
            snapshot, phases = fixture.saved()
            row = edit_record(snapshot)
            assert (row is None) == (point == "before_edit_reply")
            assert row is None or not row["dispatched"]
            assert fixture.file.read_bytes() == OLD and not fixture.artifacts()
            assert "active writer" in fixture.command(2, resume=True, driver=False, data=b"")
        finally:
            finish(process, kill=True)
        output = fixture.command(resume=True, data=b"y\n")
        assert "Apply this edit?" in output and fixture.file.read_bytes() == NEW, output
        assert fixture.saved()[1].count("dispatch") == 2

    for point, expected, observed in (
        ("after_edit_dispatch", OLD, "observed file matches expected before state"),
        ("before_edit_result", NEW, "observed bytes match expected after state"),
        ("after_edit_result", NEW, f"applied {FILE}"),
    ):
        fixture = new(point)
        process = fixture.paused(point)
        try:
            wait_for(process, f"[checkpoint] {point}".encode())
            assert fixture.file.read_bytes() == expected, point
        finally:
            finish(process, kill=True)
        identity = fixture.file.stat().st_ino
        known = point == "after_edit_result"
        output = fixture.command(0 if known else 1, resume=True, data=b"y\n")
        assert observed in output and "Apply this edit?" not in output, output
        assert fixture.file.read_bytes() == expected and fixture.file.stat().st_ino == identity
        if known:
            assert not fixture.artifacts()
        else:
            assert "effect_unknown" in output and not fixture.saved()[0]["finished"]
            assert "effect unknown" in fixture.command(1, resume=True, data=b"")
            fixture.command(resume=True, extra=("--accept-unknown",), data=b"")
            assert "accept_unknown" in fixture.saved()[1]
        assert fixture.saved()[1].count("dispatch") == 2
        assert fixture.file.read_bytes() == expected and fixture.file.stat().st_ino == identity

    for point, expected in (("before_edit_dispatch", OLD), ("before_edit_result", NEW)):
        fixture = new("disk_" + point)
        output = fixture.command(1, LAY_TEST_FULL=point)
        assert "storage_error" in output and "disk is full" in output, output
        assert fixture.file.read_bytes() == expected
        if expected == OLD:
            assert not edit_record(fixture.saved()[0])["dispatched"]
        else:
            assert f"applied {FILE}" not in output
            assert "observed bytes match expected after state" in fixture.command(1, resume=True, data=b"")

    identity = new("identity")
    identity.command(data=b"fix fixture\nn\n")
    identity.root.rename(identity.root.with_name("old workspace"))
    identity.root.mkdir()
    identity.file.write_bytes(OLD)
    assert "session scope changed" in identity.command(2, resume=True, data=b"")
    assert identity.file.read_bytes() == OLD

    future = new("future")
    future.command(data=b"fix fixture\nn\n")
    with closing(sqlite3.connect(future.db)) as db:
        db.execute("PRAGMA user_version=4")
        db.commit()
    assert "unsupported session database format" in future.command(2, resume=True, data=b"")
    with closing(sqlite3.connect(future.db)) as db:
        assert db.execute("PRAGMA user_version").fetchone()[0] == 4

    migration = new("migration")
    process = migration.paused("after_edit_dispatch")
    try:
        wait_for(process, b"[checkpoint] after_edit_dispatch")
        locked = subprocess.run([cli, "--migrate-session", "migration"], env=migration.env,
                                capture_output=True, timeout=5)
        assert locked.returncode == 2 and b"active writer" in locked.stderr, locked.stderr
    finally:
        finish(process, kill=True)
    old, checkpoints = migration.saved()
    del old["model_attempts"]
    del old["tasks"]
    payload = json.dumps(old)
    with closing(sqlite3.connect(migration.db)) as db:
        db.execute("UPDATE session SET snapshot=?", (payload,))
        db.execute("PRAGMA user_version=1")
        db.commit()
    assert "--migrate-session" in migration.command(2, resume=True, data=b"", driver=False)
    migrated = subprocess.run([cli, "--migrate-session", "migration"], env=migration.env,
                              capture_output=True, timeout=5)
    assert migrated.returncode == 0 and b"Migrated session to format 3" in migrated.stdout, migrated
    backup, = migration.db.parent.glob("session-migration.v1-*.sqlite3")
    with closing(sqlite3.connect(backup)) as db:
        assert db.execute("PRAGMA user_version").fetchone()[0] == 1
        assert db.execute("SELECT snapshot FROM session").fetchone()[0] == payload
        assert db.execute("PRAGMA quick_check").fetchone()[0] == "ok"
    updated, phases = migration.saved()
    tasks = updated.pop("tasks")
    assert tasks[0]["id"] == 1 and tasks[0]["question"] == old["question"] and not tasks[0]["closed"]
    assert updated.pop("model_attempts") == [] and updated == old
    assert phases == checkpoints + ["migrated_v1_to_v3"]
    assert "effect_unknown" in migration.command(1, resume=True, data=b"", driver=False)
    assert migration.file.read_bytes() == OLD and migration.saved()[1].count("dispatch") == 2
    assert "effect_unknown" in migration.command(2, extra=("--follow-up", "Inspect recovered file"), data=b"")
    migration.command(resume=True, extra=("--accept-unknown", "--close-task"), data=b"")
    migration.command(extra=("--follow-up", "Inspect recovered file", "--no-write"), data=b"")
    assert migration.saved()[0]["tasks"][0]["closed"] and migration.file.read_bytes() == OLD

    for status in ("finished", "pending", "unknown"):
        legacy = new("legacy-v2-" + status)
        if status == "unknown":
            process = legacy.paused("after_edit_dispatch")
            try:
                wait_for(process, b"[checkpoint] after_edit_dispatch")
            finally:
                finish(process, kill=True)
            assert "effect_unknown" in legacy.command(1, resume=True, driver=False, data=b"")
        else:
            legacy.command(0 if status == "finished" else 1, data=b"fix fixture\ny\n",
                           driver=status == "finished")
        old, _ = legacy.saved()
        del old["tasks"]
        payload = json.dumps(old)
        with closing(sqlite3.connect(legacy.db)) as db:
            db.execute("UPDATE session SET snapshot=?", (payload,))
            db.execute("PRAGMA user_version=2")
            db.execute("CREATE TRIGGER fail_upgrade BEFORE INSERT ON checkpoints "
                       "BEGIN SELECT RAISE(ABORT, 'fixture rejected the migration checkpoint'); END")
            db.commit()
        assert "--migrate-session" in legacy.command(2, resume=True, data=b"")
        migrate = [cli, "--migrate-session", "legacy-v2-" + status]
        rejected = subprocess.run(migrate, env=legacy.env, capture_output=True, timeout=5)
        assert rejected.returncode == 2 and b"fixture rejected the migration checkpoint" in rejected.stderr, rejected
        with closing(sqlite3.connect(legacy.db)) as db:
            assert db.execute("PRAGMA user_version").fetchone()[0] == 2
            assert db.execute("SELECT snapshot FROM session").fetchone()[0] == payload
            db.execute("DROP TRIGGER fail_upgrade")
        result = subprocess.run(migrate, env=legacy.env, capture_output=True, timeout=5)
        assert result.returncode == 0, result.stderr
        updated, _ = legacy.saved()
        task, = updated.pop("tasks")
        assert updated == old and task["id"] == 1 and task["closed"] == (status == "finished")
        if status == "pending":
            assert "pending tools" in legacy.command(2, extra=("--follow-up", "Read again"), data=b"")
        if status == "unknown":
            assert task["stop"] == old["stop"] and edit_record(updated)["dispatched"]
            assert "effect_unknown" in legacy.command(2, extra=("--follow-up", "Read again"), data=b"")


if __name__ == "__main__":
    with TemporaryDirectory(prefix="lay-code-edit-") as path:
        cases(Path(path), sys.argv[1], sys.argv[2])
    print("Verified approvals, edit preservation, writer exclusion, and forced-crash recovery.")
