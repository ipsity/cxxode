"""Bind the P19 automated gates and any observed desktop evidence to the frozen RC12 ZIP."""

import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET
import zipfile

import p13_receipt as frozen


OUT = Path("out")
PREFIX = "p19-rc12"
NAME = "lay-code-0.1.0-rc12-windows-x64"


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def recorded(suffix):
    return read(OUT / f"{PREFIX}-{suffix}.json")


def artifact():
    directory = OUT / NAME
    archive = OUT / f"{NAME}.zip"
    manifest = read(directory / "manifest.json")
    archive_sha = digest(archive.read_bytes())
    assert archive.with_suffix(".zip.sha256").read_text().split()[0] == archive_sha
    with zipfile.ZipFile(archive) as packed:
        assert packed.read(f"{NAME}/manifest.json") == (directory / "manifest.json").read_bytes()
        for name, metadata in manifest["binaries"].items():
            data = packed.read(f"{NAME}/{name}")
            assert len(data) == metadata["bytes"] and digest(data) == metadata["sha256"]
            assert data == (directory / name).read_bytes()
    assert manifest["version"] == "0.1.0-rc12"
    return manifest, archive_sha


def contract(platform):
    path = OUT / f"{PREFIX}-{platform}-tests.xml"
    suite = ET.parse(path).getroot()
    count = int(suite.attrib["tests"])
    assert count == 13 and not any(int(suite.get(key, 0)) for key in ("disabled", "skipped", "errors", "failures"))
    return {"passed": count, "total": count, "junit_sha256": digest(path.read_bytes())}


def latency(exe, journey):
    interactions = recorded("interaction-gates")
    native = recorded("startup-native")["samples_ms"]
    line = recorded("startup-line")["warm"]
    assert interactions["executable_sha256"] == exe and len(native) == 101 and line["count"] == 100
    busy = journey["busy_input"]
    assert busy["feedback_seen_by_conpty"]["count"] == 20
    measured = {
        "line_hot_start": (line["p95_ms"], 100),
        "native_conpty_hot_start": (frozen.distribution(native[1:])["p95_ms"], 150),
        "draft_echo": (interactions["startup"]["draft_input"]["p95_ms"], 50),
        "fixture_preview": (interactions["complete_preview_event_to_terminal"]["p95_ms"], 50),
        "running_command_cancel": (interactions["process"]["cancel_feedback"]["p95_ms"], 100),
        "busy_supplement_feedback": (busy["feedback_seen_by_conpty"]["p95_ms"], 100),
    }
    flood_log = (OUT / f"{PREFIX}-tui-performance.log").read_bytes()
    assert b"TimeoutError" in flood_log
    return {name: {"p95_ms": p95, "limit_ms": limit, "pass": p95 <= limit}
            for name, (p95, limit) in measured.items()} | {
                "busy_supplement_durable_ack": busy["durable_acknowledgment_seen_by_conpty"],
                "busy_supplement_next_post": busy["next_post_at_loopback"],
                "standalone_flood_capture": {"result": "timeout", "p19_gate": False,
                                             "log_sha256": digest(flood_log)},
                "note": "ConPTY observes the frame or output bytes. The internal host-write measurements are reported in P18.",
            }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--human-observations", type=Path)
    args = parser.parse_args()
    manifest, archive_sha = artifact()
    exe = manifest["binaries"]["lay-code.exe"]["sha256"]
    guest = recorded("clean-guest")
    assert guest["archive_sha256"] == archive_sha and guest["executable_sha256"] == exe
    assert guest["result"] == "pass" and guest["filesystem"] == "NTFS" and not guest["toolchain_commands_present"]
    assert all(guest[key] for key in ("started", "workspace_read", "edit_requires_approval", "tui_follow_up",
                                     "shared_session_reopened", "tui_two_files_verified", "cli_reopened_two_file_verification"))
    journey = recorded("journey-and-input")
    assert journey["executable_sha256"] == exe and journey["read_only"]["changed_files"] == []
    assert journey["read_only"]["requests"] == 2 and journey["read_only"]["approval_count"] == 0
    assert len(journey["edit_journeys"]) == 2 and all(case["tasks"] == 2 and
        case["passing_verification_revision"] == case["failed_verification_revision"] + 1 and
        case["extra_posts_while_browsing_or_inspecting"] == 0 for case in journey["edit_journeys"])
    assert journey["repeated_failure"] == {"paused_and_closed": True, "requests": 2, "tool_dispatches": 2,
                                           "additional_posts_after_close": 0}
    frozen.PREFIX = PREFIX
    quality = frozen.quality(exe, manifest["build"])
    primary = read(OUT / f"{PREFIX}-live" / "summary.json")
    quality["failed_samples"] = [{key: row[key] for key in ("case", "repeat", "cli_exit", "requests", "checked")} |
        {"harness_status": row.get("harness_status")} for row in primary["attempts"] if not row["passed"]]
    rates = latency(exe, journey)
    native = recorded("native-desktop")
    readerpath = OUT / f"{PREFIX}-reader.json"
    reader = read(readerpath) if readerpath.is_file() else {"result": "pending"}
    if readerpath.is_file():
        assert reader["executable_sha256"] == exe and reader["result"] in ("pass", "gap")
    assert native["executable_sha256"] == exe and native["archive_sha256"] == archive_sha
    assert native["snapshot"]["requests"] == 5 and native["snapshot"]["tasks"] == 3
    assert native["snapshot"]["edit_dispatches"] == native["snapshot"]["run_dispatches"] == 1
    assert native["snapshot"]["run_results"] == ["command cancelled"]
    human = read(args.human_observations) if args.human_observations else {"result": "pending"}
    if args.human_observations:
        assert human["archive_sha256"] == archive_sha and human["result"] in ("pass", "fail")
        if human["result"] == "pass":
            assert all(human["checks"][key] == "pass" for key in (
                "microsoft_pinyin", "native_multiline_paste", "narrow_approval", "cancel_and_reopen"))
    automation = quality["passed"] >= quality["threshold"] and not any(quality["guardrails"].values()) and all(
        item["pass"] for item in rates.values() if isinstance(item, dict) and "pass" in item)
    report = {
        "milestone": "P19", "release": NAME, "archive_sha256": archive_sha,
        "compiler": "MinGW UCRT GCC 15.2.0 Rev13",
        "build_options": {"type": "Release", "workspace": True, "benchmark_metrics": False},
        "manifest": manifest,
        "automatic_gate": "pass" if automation else "fail",
        "contracts": {"windows_same_executable": contract("windows"), "wsl_independent_build": contract("wsl")},
        "clean_guest": guest, "journeys": journey, "performance": rates, "online_quality": quality,
        "native_terminal_scripted_on_real_desktop": native, "windows_screen_reader": reader,
        "human_observations": human,
        "desktop_procedure": "bench/P19_MANUAL_WINDOWS.md",
        "status": "signed-off" if automation and human["result"] == "pass" and reader["result"] in ("pass", "gap") else "pending",
    }
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return int(not automation)


if __name__ == "__main__":
    raise SystemExit(main())
