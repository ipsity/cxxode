"""Stage and smoke-test the x64 MinGW portable release."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
from tempfile import TemporaryDirectory
import zipfile


REPO = Path(__file__).resolve().parents[1]
SYSTEM = set("advapi32 bcrypt crypt32 iphlpapi kernel32 msvcrt ntdll secur32 shell32 "
             "user32 ucrtbase version winmm wldap32 ws2_32 normaliz".split())
MINGW = {
    "libgcc_s_seh-1.dll": "gcc-libs",
    "libstdc++-6.dll": "gcc-libs",
    "libwinpthread-1.dll": "winpthreads",
    "libsqlite3-0.dll": "sqlite3",
}


def hash_file(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def imports(objdump, binary):
    inspected = subprocess.run([objdump, "-p", binary.name], cwd=binary.parent, capture_output=True, check=True)
    if b"file format pei-x86-64" not in inspected.stdout:
        raise ValueError(f"{binary} is not an x64 PE image")
    return {name.decode("ascii").lower() for name in re.findall(rb"DLL Name:\s*([^\s]+)", inspected.stdout)}


def dependencies(objdump, app, mingw, curl):
    queues = [app]
    libraries = {}
    while queues:
        for name in sorted(imports(objdump, queues.pop())):
            if name.startswith(("api-ms-win-", "ext-ms-win-")) or Path(name).stem in SYSTEM or name in libraries:
                continue
            if name in MINGW:
                path = mingw / "bin" / name
            elif name in {"libcurl.dll", "libcurl-4.dll"}:
                path = curl / "bin" / name
            else:
                raise ValueError(f"Unlisted runtime dependency {name}")
            libraries[name] = path.resolve(strict=True)
            queues.append(libraries[name])
    return libraries


def clean_environment(state):
    windows = Path(os.environ["SystemRoot"])
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join(map(str, (windows / "System32", windows)))
    env["LOCALAPPDATA"] = str(state)
    env.pop("ANTHROPIC_API_KEY", None)
    return env


def smoke(application):
    with TemporaryDirectory(prefix="lay-code-portable-") as temporary:
        base = Path(temporary)
        workspace = base / "安装验证"
        workspace.mkdir()
        fixture = workspace / "config.txt"
        fixture.write_bytes(b"empty_config = reject\n")
        state = base / "private"
        env = clean_environment(state)
        help_page = subprocess.run([application, "--help"], env=env, cwd=base, capture_output=True, timeout=10)
        if help_page.returncode or b"Usage: lay-code" not in help_page.stdout:
            raise RuntimeError(f"Package startup failed ({help_page.returncode}): {help_page.stderr!r}")
        read = subprocess.run([
            application, "--workspace", workspace, "--model", "fixture-edit", "--session", "portable-smoke",
            "--write", "config.txt", "--question", "fix fixture",
        ], env=env, cwd=base, input=b"", capture_output=True, timeout=10)
        if (read.returncode != 1 or b"[read] config.txt (ok)" not in read.stdout or
                b"approval_required" not in read.stdout or fixture.read_bytes() != b"empty_config = reject\n"):
            raise RuntimeError(f"Package workspace read failed ({read.returncode}): {read.stdout!r} {read.stderr!r}")
        db = sqlite3.connect(state / "LayCode/sessions/session-portable-smoke.sqlite3")
        try:
            snapshot = json.loads(db.execute("SELECT snapshot FROM session").fetchone()[0])
            evidence = snapshot["rounds"][0]["tools"][0]["result"]
            if not evidence.startswith("config.txt\n1: empty_config = reject\n"):
                raise RuntimeError("Packaged application did not record the expected file content")
        finally:
            db.close()


def package(args):
    app = args.exe.resolve(strict=True)
    mingw = args.mingw.resolve(strict=True)
    curl = args.curl.resolve(strict=True)
    if not re.fullmatch(r"[0-9][0-9a-z.-]*", args.version):
        raise ValueError("Invalid package version")
    libraries = dependencies(mingw / "bin/objdump.exe", app, mingw, curl)
    folder = f"lay-code-{args.version}-windows-x64"
    stage = args.output.resolve() / folder
    archive = stage.parent / f"{folder}.zip"
    if stage.exists() or archive.exists():
        raise FileExistsError("Package output already exists")
    stage.mkdir(parents=True)
    sources = {"lay-code.exe": app, **libraries}
    for name, source in sources.items():
        shutil.copyfile(source, stage / name)
    for relative in ("README.md", "RELEASE_NOTES.md", "packaging/setup_provider.ps1"):
        source = REPO / relative
        shutil.copyfile(source, stage / source.name)
    licenses = stage / "licenses"
    licenses.mkdir()
    for group in sorted({MINGW[name] for name in libraries if name in MINGW}):
        shutil.copytree(mingw / "share/licenses" / group, licenses / group)
    shutil.copyfile(args.curl_license, licenses / "curl.txt")
    shutil.copyfile(args.json_license, licenses / "nlohmann-json.txt")
    smoke(stage / "lay-code.exe")
    manifest = {
        "version": args.version,
        "platform": "Windows x64, MinGW UCRT",
        "build": args.build,
        "curl_source_sha256": hash_file(args.curl_source),
        "smoke_test": "Local Windows with System32-only toolchain PATH, actual NTFS workspace read",
        "binaries": {name: {"sha256": hash_file(stage / name), "bytes": (stage / name).stat().st_size}
                     for name in sorted(sources)},
    }
    (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as packed:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                packed.write(path, arcname=path.relative_to(stage.parent).as_posix())
    with zipfile.ZipFile(archive) as packed, TemporaryDirectory(prefix="lay-code-unpacked-") as temporary:
        packed.extractall(temporary)
        smoke(Path(temporary) / folder / "lay-code.exe")
    checksum = hash_file(archive)
    archive.with_suffix(".zip.sha256").write_text(f"{checksum}  {archive.name}\n", encoding="ascii")
    print(f"{archive}\n{checksum}\nPackaged and verified the extracted application.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "mingw", "curl", "curl-license", "json-license", "curl-source", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("--version", default="0.1.0-rc1")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Package smoke tests run on Windows")
    package(args)


if __name__ == "__main__":
    main()
