"""Run the same two-task live fixture through the line CLI and Windows TUI."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import time

from winpty import Backend, PtyProcess

from conpty_quality import pending_action
from live_quality import context


COMMAND = ["--output-on-failure", "-R", "^empty_config$"]
INITIAL = b"empty_config = reject\n"
UPDATED = b"empty_config = allow\n"
FIRST = ("先用 ctest --output-on-failure -R ^empty_config$ 运行用例并记录失败。"
         "再只改 config.txt，把 empty_config = reject 改成 empty_config = allow。改后本任务先不复测。")
FOLLOW_UP = ("保留刚才的修改，不编辑文件。只用 ctest --output-on-failure -R ^empty_config$"
             " 复跑刚才失败的用例，报告验证结果。")
LINE_PROMPTS = ("Apply this edit? [y/N] ", "Run this command? [y/N] ", "[session] ")
TUI_APPROVALS = ("\x1b[2;1H\x1b[2K待批准 · 编辑", "\x1b[2;1H\x1b[2K待批准 · 运行")
TUI_PROMPTS = (*TUI_APPROVALS, "已完成", "任务暂停", "会话错误", "效果未知")


class Terminal:
    def __init__(self, command, environment):
        self.process = PtyProcess.spawn(list(map(str, command)), env=environment,
                                        dimensions=(32, 155), backend=Backend.ConPTY)
        self.pending = ""

    def wait(self, *markers):
        deadline = time.monotonic() + 200
        while time.monotonic() < deadline:
            for marker in markers:
                where = self.pending.find(marker)
                if where >= 0:
                    self.pending = self.pending[where + len(marker):]
                    return marker
            if not select.select([self.process.fileobj], [], [], 0.05)[0]:
                if not self.process.isalive():
                    break
                continue
            try:
                self.pending += self.process.read(32768)
                self.pending = self.pending[-32768:]
            except EOFError:
                break
        raise RuntimeError(f"Terminal did not show any of {markers!r}. Tail: {self.pending[-2000:]!r}")

    def send(self, keys):
        self.process.write(keys)

    def finish(self, exit_window=False):
        if exit_window:
            self.send("\x1b")
        deadline = time.monotonic() + 6
        while self.process.isalive() and time.monotonic() < deadline:
            if select.select([self.process.fileobj], [], [], 0.05)[0]:
                try:
                    self.process.read(32768)
                except EOFError:
                    break
        if self.process.isalive() or self.process.exitstatus != 0:
            raise RuntimeError(f"Unexpected fixture process result {self.process.exitstatus}")

    def close(self):
        self.process.close(force=True)


def approve(tool, request, task, workspace, ctest):
    if tool == "edit":
        if task != 1 or request.get("path") != "config.txt":
            return False
        old, new = request.get("old_text", "").encode(), request.get("new_text", "").encode()
        current = (workspace / "config.txt").read_bytes()
        return current == INITIAL and old and current.count(old) == 1 and current.replace(old, new, 1) == UPDATED
    program = str(request.get("program", "")).replace("\\", "/").lower()
    return (program in ("ctest", "ctest.exe", ctest.as_posix().lower()) and request.get("args") == COMMAND and
            request.get("path", ".") in ("", "."))


def task(terminal, use_tui, index, database, workspace, ctest, decisions):
    while True:
        marker = terminal.wait(*(TUI_PROMPTS if use_tui else LINE_PROMPTS))
        if marker in ("已完成", "[session] "):
            return
        if marker not in (*TUI_APPROVALS, *LINE_PROMPTS[:2]):
            raise RuntimeError(f"Task did not finish: {marker}")
        tool = "edit" if marker in (TUI_APPROVALS[0], LINE_PROMPTS[0]) else "run"
        request = pending_action(database, tool)
        granted = bool(approve(tool, request, index, workspace, ctest))
        decisions.append({"task": index, "tool": tool, "approved": granted})
        if use_tui:
            terminal.wait("Tab 输入决定")
            terminal.send("\t")
            terminal.wait("空回车拒绝")
            if granted:
                terminal.send("y")
                terminal.wait("> y")
            terminal.send("\r")
        else:
            terminal.send("y\r" if granted else "\r")


def run(args, mode):
    base = args.output / mode
    workspace, state = base / "fixture", base / "private"
    workspace.mkdir(parents=True)
    check = Path(__file__).resolve().parents[1] / "tests/fixtures/empty-config/check.cmake"
    shutil.copyfile(check, workspace / "check.cmake")
    (workspace / "config.txt").write_bytes(INITIAL)
    (workspace / "sample.conf").touch()
    (workspace / "CTestTestfile.cmake").write_text(
        f'add_test(empty_config [==[{args.cmake.as_posix()}]==] -P check.cmake)\n', encoding="utf-8")
    environment = {**os.environ, "LOCALAPPDATA": str(state),
                   "PATH": str(args.ctest.parent) + os.pathsep + os.environ.get("PATH", "")}
    flags = ["--workspace", workspace, "--session", "paired", "--write", "config.txt", "--allow-run",
             "--provider", args.provider, "--model", args.model, "--provider-config", args.provider_config]
    database = state / "LayCode/sessions/session-paired.sqlite3"
    decisions = []
    for index, question in enumerate((FIRST, FOLLOW_UP), 1):
        tui = mode == "tui"
        if tui:
            command = [args.cli, "--tui", "--mono", *flags, *(["--resume"] if index == 2 else [])]
        else:
            command = [args.cli, *flags, *(["--question", question] if index == 1 else
                                          ["--follow-up", question, "--no-write"])]
        terminal = Terminal(command, environment)
        try:
            if tui:
                terminal.wait("等待任务" if index == 1 else "已完成")
                if index == 2:
                    terminal.send("\x1bOS")
                    terminal.wait("后续任务已禁用编辑工具")
                terminal.send(question)
                terminal.wait(question[-10:])
                terminal.send("\x1bOQ")
            task(terminal, tui, index, database, workspace, args.ctest, decisions)
            terminal.finish(tui)
        finally:
            terminal.close()
        print(f"{mode} task {index} finished", flush=True)
    session = context(database)
    first = session["tasks"][1]["first_round"]
    rounds = [[record for item in group for record in item["tools"] if record["dispatched"]]
              for group in (session["rounds"][:first], session["rounds"][first:])]
    checks = [[{"arguments": tool["run"]["arguments"], "code": tool["verification"]["code"],
                "test_summary": tool["verification"]["test_summary"]} for tool in group if tool.get("verification")]
              for group in rounds]
    attempts = session["model_attempts"]
    usage = {}
    for field in ("input_tokens", "output_tokens"):
        observed = [item[field] for item in attempts if item[field] is not None]
        usage[field] = {"known_total": sum(observed), "attempts_without_count": session["requests"] - len(observed)}
    passed = ((workspace / "config.txt").read_bytes() == UPDATED and [item["name"] for item in rounds[1]] == ["run"] and
              len(checks[0]) == len(checks[1]) == 1 and checks[0][0]["code"] != 0 and checks[1][0]["code"] == 0 and
              all(item["arguments"] == COMMAND for group in checks for item in group))
    return {"passed": passed, "checks": checks, "dispatches": [[item["name"] for item in group] for group in rounds],
            "requests": session["requests"], "first_byte_us": [item["first_byte_us"] for item in attempts],
            "usage": usage, "approval_decisions": decisions}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cli", "provider-config", "cmake", "ctest", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--provider", required=True)
    parser.add_argument("--model", required=True)
    args = parser.parse_args()
    for field in ("cli", "provider_config", "cmake", "ctest", "output"):
        setattr(args, field, getattr(args, field).resolve())
    args.output.mkdir()
    report = {"provider": args.provider, "model": args.model, "protocol": "responses",
              "executable_sha256": hashlib.sha256(args.cli.read_bytes()).hexdigest(),
              "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()}
    for mode in ("line", "tui"):
        report[mode] = run(args, mode)
        (args.output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if all(report[key]["passed"] for key in ("line", "tui")) else 1


if __name__ == "__main__":
    raise SystemExit(main())
