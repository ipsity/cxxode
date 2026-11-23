import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
import time

from editing import Fixture, finish, wait_for


def process_contract(driver, directory):
    helper = Path(__file__).resolve().parents[1] / "fixtures" / "process_child.py"
    env = {**os.environ, "ANTHROPIC_API_KEY": "TEST_SECRET"}

    def child(mode, *args, timeout=10000, cancel=0, executable=sys.executable):
        command = [driver, directory, executable, str(timeout), str(cancel)]
        if mode:
            command.extend([helper, mode, *args])
        result = subprocess.run(command, capture_output=True, env=env, timeout=12)
        assert result.returncode == 0, (command, result.returncode, result.stderr)
        return json.loads(result.stdout)

    arguments = ["", "two words", '嵌入"引号', "tail\\", "slash\\\\\"quote", "single'quote",
                 "line\nbreak", "& no shell"]
    result = child("arguments", *arguments)
    assert result["status"] == "command exited 0", result
    report = json.loads(result["stdout"]["head"])
    assert report == {"argv": arguments, "cwd": str(directory), "secret": None, "stdin_closed": True}, report

    result = child("flood")
    assert result["status"] == "command exited 0", result["diagnostic"]
    for name, marker, end in (
        ("stdout", "STDOUT", b"\x1b[31mSTDOUT-END\xff\n"),
        ("stderr", "STDERR", b"STDERR-END\n"),
    ):
        captured = result[name]
        total = 800 * 65536 + len(marker + "-START\n") + len(end)
        assert captured["total"] == total and captured["omitted"] == total - 65536
        assert captured["head"].startswith(marker + "-START\n")
        assert marker + "-END" in captured["tail"]
    assert "\x1b" not in result["stdout"]["tail"] and r"\x1b" in result["stdout"]["tail"]
    assert r"\xff" in result["stdout"]["tail"]

    result = child("linger")
    assert result["status"] == "command exited 0" and result["descendants_terminated"], result
    assert "child started" in result["stdout"]["head"] and result["elapsed_ms"] < 3200, result
    for param, expected in (({"timeout": 180}, "command timed out"), ({"cancel": 180}, "command cancelled")):
        result = child("hold", **param)
        assert result["status"] == expected and result["elapsed_ms"] < 3200, result
    result = child("exit127")
    assert result["status"] == "command exited 127", result
    result = child(None, executable=directory / "missing.exe")
    assert result["status"] == "command could not be started", result


class VerificationFixture(Fixture):
    def __init__(self, base, name, cli, driver, cmake, ctest, *, fails=False, no_tests=False, hold=False):
        super().__init__(base, name, cli, driver, data=b"empty_config = reject\n")
        self.args[self.args.index("fixture-edit")] = "fixture-verify"
        self.args += ["--allow-run"]
        self.env["PATH"] = str(Path(ctest).parent) + os.pathsep + self.env.get("PATH", "")
        source = Path(__file__).resolve().parents[1] / "fixtures" / "empty-config"
        shutil.copyfile(source / "check.cmake", self.root / "check.cmake")
        (self.root / "sample.conf").touch()
        options = (f'[==[-DSETTINGS_FILE={self.file.name}]==] "-DRECORD_ATTEMPT=ON" '
                   + ('"-DFORCE_FAILURE=ON" ' if fails else '')
                   + ('"-DHOLD_TEST=ON" ' if hold else ''))
        (self.root / "CTestTestfile.cmake").write_text(
            "" if no_tests else
            f'add_test(empty_config [==[{Path(cmake).as_posix()}]==] {options}"-P" "check.cmake")\n',
            encoding="utf-8",
        )

    def count(self):
        record = self.root / "observed-runs.txt"
        return len(record.read_text().splitlines()) if record.exists() else 0


def verification_contract(base, cli, driver, cmake, ctest):
    def new(name, **kwargs):
        return VerificationFixture(base, name, cli, driver, cmake, ctest, **kwargs)

    def results(fixture):
        return [tool for round in fixture.saved()[0]["rounds"] for tool in round["tools"] if tool["name"] == "run"]

    approvers = b"fix and verify\ny\ny\ny\n"
    success = new("verified")
    output = success.command(data=approvers)
    assert output.count("Run this command? [y/N] ") == 2 and success.count() == 2, output
    assert success.file.read_bytes() == b"empty_config = allow\n"
    run_before, run_after = results(success)
    assert run_before["verification"]["code"] != 0 and run_after["verification"]["code"] == 0
    assert "CTest reported 1 failed" in output and "CTest reported 1 passing test(s)." in output, output
    assert "[verification] command exited 0 at edit revision 1\n" in output, output
    assert [run["run"]["edit_revision"] for run in (run_before, run_after)] == [0, 1]
    assert run_after["run"]["arguments"] == ["--output-on-failure", "-R", "^empty_config$"]
    assert run_after["verification"]["observed_utc"].endswith("Z")
    assert "CTest reported 1 passing" in success.command(resume=True, driver=False, data=b"")
    assert success.count() == 2
    follow_up = "保留修改，只运行前一次失败的测试"
    output = success.command(1, driver=False, extra=("--follow-up", follow_up, "--no-write"), data=b"")
    assert "approval_required" in output and success.count() == 2
    output = success.command(resume=True, data=b"y\n")
    assert f"[task 2] {follow_up}" in output and output.count("Run this command? [y/N] ") == 1
    assert "Apply this edit?" not in output and success.count() == 3
    snapshot, phases = success.saved()
    first, second = snapshot["tasks"]
    assert (first["id"], second["id"], second["allow_edit"], second["closed"]) == (1, 2, False, True)
    assert phases.count("follow_up") == 1
    assert first["end_round"] == second["first_round"]
    assert [record["name"] for r in snapshot["rounds"][second["first_round"]:] for record in r["tools"]] == ["run"]
    assert results(success)[-1]["run"]["arguments"] == run_before["run"]["arguments"]
    assert success.file.read_bytes() == b"empty_config = allow\n"

    process = success.paused("before_run_result", data=b"y\n",
                             extra=("--follow-up", "再次运行同一用例", "--no-write"))
    try:
        wait_for(process, b"[checkpoint] before_run_result")
        assert success.count() == 4 and results(success)[-1]["dispatched"]
    finally:
        finish(process, kill=True)
    output = success.command(1, resume=True, data=b"y\n")
    assert "effect_unknown" in output and "Run this command?" not in output and success.count() == 4

    missing = new("missing")
    output = missing.command(data=b"verify with missing program\ny\n")
    assert output.count("command could not be started") == 2 and missing.count() == 0, output
    assert "passing test" not in output and not any(run["dispatched"] for run in results(missing))

    failed = new("still-fails", fails=True)
    output = failed.command(data=approvers)
    assert output.count("CTest reported 1 failed") == 2 and failed.count() == 2, output
    assert failed.file.read_bytes() == b"empty_config = allow\n"
    assert "passing test" not in output

    no_tests = new("no-tests", no_tests=True)
    output = no_tests.command(data=approvers)
    assert output.count("CTest reported no tests.") == 2 and "passing test" not in output, output

    stale = new("edited-again")
    output = stale.command(data=b"verify and edit again\ny\ny\ny\ny\n")
    assert "[verification] command exited 0 at edit revision 1 (earlier workspace revision)" in output, output
    assert stale.file.read_bytes() == b"empty_config = review\n"

    denied = new("run-denied")
    output = denied.command(data=b"fix and verify\nn\ny\nn\n")
    assert output.count("command denied by user") == 2 and denied.count() == 0, output
    assert not any(run["dispatched"] for run in results(denied))
    output = denied.command(extra=("--follow-up", "仅重复同一个测试", "--no-write"), data=b"")
    assert "Run this command?" not in output and output.count("command denied by user") == 3
    assert denied.count() == 0 and not any(run["dispatched"] for run in results(denied))
    noninteractive = new("run-piped")
    assert "approval_required" in noninteractive.command(1, driver=False, data=approvers)
    assert noninteractive.count() == 0 and not results(noninteractive)[0]["dispatched"]

    for point, run_count in (("after_run_dispatch", 0), ("before_run_result", 1)):
        fixture = new(point)
        process = fixture.paused(point, data=b"fix and verify\ny\n")
        try:
            wait_for(process, f"[checkpoint] {point}".encode())
            assert fixture.count() == run_count and results(fixture)[0]["dispatched"]
        finally:
            finish(process, kill=True)
        output = fixture.command(1, resume=True, data=b"y\n")
        assert "effect_unknown" in output and "Its effects and cleanup are unknown." in output, output
        assert "Run this command?" not in output and fixture.count() == run_count
        assert fixture.saved()[1].count("dispatch") == 1
        assert results(fixture)[0]["verification"]["test_summary"] == ""

    active = new("crash-during-run", hold=True)
    process = active.paused("unused", data=b"fix and verify\ny\n")
    try:
        deadline = time.monotonic() + 10
        while active.count() == 0 and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        assert active.count() == 1 and process.poll() is None
        interrupted = time.monotonic()
    finally:
        finish(process, kill=True)
    output = active.command(1, resume=True, data=b"y\n")
    assert "effect_unknown" in output and active.count() == 1, output
    assert active.saved()[1].count("dispatch") == 1
    time.sleep(max(0, interrupted + 3.3 - time.monotonic()))
    if os.name == "nt":
        assert not (active.root / "observed-completion.txt").exists(), "a Windows Job child survived host termination"


if __name__ == "__main__":
    program, driver, process_driver, cmake, ctest = sys.argv[1:]
    with TemporaryDirectory(prefix="lay-code-run-") as path:
        base = Path(path).resolve()
        directory = base / "进程 工作区"
        directory.mkdir()
        process_contract(process_driver, directory)
        verification_contract(base, program, driver, cmake, ctest)
    print("Verified direct execution, bounded output, process cleanup, and command recovery.")
