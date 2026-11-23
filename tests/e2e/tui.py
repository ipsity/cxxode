"""Exercise the offline TUI in a PTY and verify that pipes stay line-oriented."""

import codecs
from contextlib import contextmanager
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import time
import unicodedata


if os.name != "nt":
    import fcntl
    import pty
    import struct
    import termios
else:
    try:
        from winpty import Backend, PtyProcess
    except ImportError:
        PtyProcess = None


ESC = "\x1b"
PAINT = re.compile(r"\x1b\[(\d+);1H\x1b\[2K(?:\x1b\[[0-9;]+m)?([^\x1b\r\n]*)")
COLOUR = re.compile(r"\x1b\[(?:31|32|36)m")


class Terminal:
    def __init__(self, output, read, send, resize, alive):
        self.output = output
        self.read = read
        self.send = send
        self.resize = resize
        self.alive = alive
        self.text = ""

    def collect(self):
        if select.select([self.output], [], [], 0.04)[0]:
            try:
                self.text += self.read()
            except (EOFError, OSError):
                pass

    def expect(self, marker, start=0):
        until = time.monotonic() + 3
        while marker not in self.text[start:] and time.monotonic() < until:
            self.collect()
        assert marker in self.text[start:], repr(self.text[start:])

    def key(self, value, marker):
        start = len(self.text)
        self.send(value)
        self.expect(marker, start)

    def quiet(self):
        until = time.monotonic() + 0.18
        while time.monotonic() < until:
            self.collect()

    def quit(self):
        if self.alive():
            self.key(ESC, ESC + "[?1049l")


@contextmanager
def console(cli, args, rows, columns, env):
    if os.name == "nt":
        process = PtyProcess.spawn([cli, *args], dimensions=(rows, columns), backend=Backend.ConPTY, env=env)
        terminal = Terminal(process.fileobj, lambda: process.read(8192), process.write, process.setwinsize,
                            process.isalive)
        try:
            yield terminal
            terminal.quit()
            deadline = time.monotonic() + 3
            while process.isalive() and time.monotonic() < deadline:
                time.sleep(0.01)
            assert not process.isalive() and process.exitstatus == 0
        finally:
            process.close(force=True)
    else:
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)

        def resize(height, width):
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))

        resize(rows, columns)
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        try:
            with subprocess.Popen([cli, *args], stdin=slave, stdout=slave, stderr=slave, env=env) as process:
                terminal = Terminal(master, lambda: decoder.decode(os.read(master, 8192)),
                                    lambda text: os.write(master, text.encode()), resize,
                                    lambda: process.poll() is None)
                try:
                    yield terminal
                    terminal.quit()
                    assert process.wait(timeout=3) == 0
                    assert termios.tcgetattr(slave) == original
                finally:
                    if process.poll() is None:
                        process.kill()
        finally:
            os.close(master)
            os.close(slave)


def matrix(cli, env):
    scenes = {
        "normal": ("已完成 · parser-demo", "1 项通过"),
        "approval": ("待批准 · 编辑 1/2", "文件版本 demo-ee91"),
        "failure": ("验证失败 · 退出码 8", "1 项失败"),
        "unknown": ("效果未知 · 等待核对", "已派发，结果缺失"),
    }
    for rows, columns in ((32, 120), (24, 80), (16, 48)):
        for name, markers in scenes.items():
            for mono in (False, True):
                args = ["--tui", "--demo", name, *(["--mono"] if mono else [])]
                with console(cli, args, rows, columns, env) as terminal:
                    for marker in (*markers, "Ctrl+C 停止", "Esc 退出"):
                        terminal.expect(marker)
                    assert bool(COLOUR.search(terminal.text)) == (not mono)
                    painted = PAINT.findall(terminal.text)
                    assert painted and all(1 <= int(row) <= rows for row, _ in painted)
                    for _, text in painted:
                        width = sum(2 if unicodedata.east_asian_width(char) in ("F", "W") else 1 for char in text)
                        assert width <= columns - 1, (columns, width, text)
                    if name == "approval" and columns == 48:
                        terminal.key(ESC + "[6~", "+ if (input.empty())")
                        terminal.key(ESC + "[5~", "1-8/")
                    if name == "failure":
                        terminal.key("\x0f", "空配置仍触发解析错误")
                    if name == "unknown":
                        terminal.key("\x0f", "自动重发 0 次")
    with console(cli, ["--tui", "--demo", "unknown", "--mono", "--ascii"], 16, 48, env) as terminal:
        terminal.expect("Effect unknown")
        terminal.expect("Esc quit")
        assert terminal.text.isascii() and not COLOUR.search(terminal.text)


def interactions(cli, env):
    with console(cli, ["--tui", "--demo", "approval", "--mono"], 24, 80, env) as terminal:
        terminal.expect("Tab 输入决定")
        terminal.send("\ty\r")
        terminal.expect("输入 y 或 yes")
        terminal.quiet()
        assert "运行 2/2" not in terminal.text and "> y" not in terminal.text
        terminal.key(ESC + "[200~y\r" + ESC + "[201~", "已忽略粘贴或排队回车")
        terminal.key("\t", "输入 y 或 yes")
        terminal.key("y", "> y")
        terminal.key("\r", "运行 2/2")
        terminal.expect("当前账户")
        terminal.expect("^parser_empty_config$")
        terminal.key("\x03", "已停止 · 审批已关闭")
        terminal.expect("待批准动作没有执行")

    with console(cli, ["--tui", "--demo", "approval", "--mono"], 16, 48, env) as terminal:
        terminal.expect("Tab 输入决定")
        terminal.key("\t", "空回车拒绝")
        terminal.key("\r", "已拒绝")

    with console(cli, ["--tui", "--mono"], 32, 120, env) as terminal:
        terminal.expect("下一条任务草稿")
        terminal.quiet()
        idle = terminal.text
        terminal.quiet()
        assert idle == terminal.text, "idle terminal redrew"
        text = "修复空配置"
        terminal.key(text, "> " + text)
        terminal.send("\r")
        terminal.quiet()
        assert "已记录草稿" not in terminal.text
        terminal.key("e\u0301", "> " + text + "e\u0301")
        terminal.key("👩\u200d💻", "> " + text + "e\u0301👩\u200d💻")
        terminal.key("\x7f", "> " + text + "e\u0301")
        terminal.key("\x7f", "> " + text)
        resized = len(terminal.text)
        terminal.resize(16, 48)
        terminal.expect("\x1b[16;1H", resized)
        terminal.expect("> " + text, resized)
        terminal.key(ESC + "OQ", "已记录草稿")
        terminal.expect("你  " + text)
        terminal.expect("未调用模型或工具")


if __name__ == "__main__":
    cli = str(Path(sys.argv[1]).resolve())
    environment = {name: value for name, value in os.environ.items() if name != "NO_COLOR"}
    piped = subprocess.run([cli, "--tui", "--demo", "approval"], input=b"y\n", capture_output=True,
                           timeout=5, env=environment)
    assert piped.returncode == 2 and b"interactive terminal" in piped.stderr and not piped.stdout
    if os.name == "nt" and PtyProcess is None:
        print("Verified pipe rejection. Install pywinpty to run the ConPTY matrix.")
        sys.exit(77)
    else:
        matrix(cli, environment)
        interactions(cli, environment)
        print("Verified four TUI scenes in three sizes, monochrome, ASCII, keys, resize and draft editing.")
