"""Prepare or verify frozen quality fixtures outside the fixture catalog."""

import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


CATALOG = Path(__file__).resolve().parents[1] / "tests/fixtures/quality/cases.json"


def git(root, *arguments):
    return subprocess.check_output([
        "git", "-C", root, "-c", "core.autocrlf=false", "-c", "core.hooksPath=.git/no-hooks",
        "-c", "commit.gpgsign=false", "-c", "user.name=Lay Code Fixture",
        "-c", "user.email=fixture@example.invalid", *arguments,
    ])


def write_files(root, files):
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))


def prepare(case, root):
    root.mkdir()
    git(root, "init", "--quiet", "--template=")
    write_files(root, case["files"])
    git(root, "add", "--force", "--", *case["files"])
    git(root, "commit", "--quiet", "-m", "Initial quality fixture")
    write_files(root, case.get("dirty", {}))


def run_assertion(case, root):
    command = "import sys; sys.path.insert(0, '.'); import app; assert " + case["assertion"]
    return subprocess.run([sys.executable, "-I", "-B", "-c", command], cwd=root,
                          capture_output=True, timeout=5)


def require(condition, case, message):
    if not condition:
        raise RuntimeError(f"{case['id']}: {message}")


def verify_case(case, root):
    prepare(case, root)
    dirty = case.get("dirty", {})
    expected_status = {
        (" M " if path in case["files"] else "?? ").encode() + path.encode()
        for path in dirty
    }
    status = git(root, "status", "--porcelain=v1", "-z", "--untracked-files=all")
    require(set(status.rstrip(b"\0").split(b"\0")) - {b""} == expected_status,
            case, "initial worktree changes differ from the catalog")
    for path, text in (case["files"] | dirty).items():
        require((root / path).read_bytes() == text.encode("utf-8"), case, f"unexpected bytes in {path}")

    if "location" in case:
        path, line, expected = case["location"]
        actual = (root / path).read_text(encoding="utf-8").splitlines()[line - 1]
        require(actual.strip() == expected, case, "location anchor moved")
    elif "assertion" in case:
        require(run_assertion(case, root).returncode != 0, case, "initial bug did not fail the assertion")
        path, = case["allow"]
        original = (root / path).read_text(encoding="utf-8")
        old, new = case["reference"]
        require(original.count(old) == 1, case, "reference patch does not match exactly once")
        write_files(root, {path: original.replace(old, new, 1)})
        result = run_assertion(case, root)
        require(result.returncode == 0, case, result.stderr.decode(errors="replace"))
        for other_path, text in dirty.items():
            if other_path not in case["allow"]:
                require((root / other_path).read_bytes() == text.encode("utf-8"),
                        case, f"user file changed: {other_path}")
    else:
        verification = case["verification"]
        missing = root / verification["missing"]
        script = verification["script"]
        if verification["status"] == "launch_failed":
            require((root / script).is_file(), case, "test script is missing too")
            try:
                subprocess.run([missing, script], cwd=root, capture_output=True, timeout=5)
            except FileNotFoundError:
                pass
            else:
                raise RuntimeError(f"{case['id']}: expected a launch failure")
        else:
            require(not missing.exists(), case, "test script unexpectedly exists")
            result = subprocess.run([sys.executable, "-B", script], cwd=root,
                                    capture_output=True, timeout=5)
            require(result.returncode == 2 and b"smoke.py" in result.stderr,
                    case, "expected Python to report a missing script")
        require(not git(root, "status", "--porcelain=v1", "--untracked-files=all"),
                case, "verification modified the fixture")


def main():
    source = json.loads(CATALOG.read_text(encoding="utf-8"))
    cases = {case["id"]: case for case in source}
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list")
    commands.add_parser("verify")
    setup = commands.add_parser("prepare")
    setup.add_argument("case", choices=cases)
    setup.add_argument("new_directory", type=Path)
    args = parser.parse_args()

    if args.command == "list":
        for case in cases.values():
            print(case["id"], case["task"])
    elif args.command == "prepare":
        case = cases[args.case]
        prepare(case, args.new_directory.resolve())
        print(case["task"])
    else:
        counts = Counter(case["id"].split("-", 1)[0] for case in source)
        if len(source) != len(cases) or counts != {"locate": 4, "fix": 4, "preserve": 2, "missing": 2}:
            raise RuntimeError("Quality catalog must contain the twelve distinct frozen cases")
        with TemporaryDirectory(prefix="lay-code-quality-") as directory:
            for case in cases.values():
                verify_case(case, Path(directory) / case["id"])
        print(f"Verified all {len(cases)} quality fixtures.")


if __name__ == "__main__":
    main()
