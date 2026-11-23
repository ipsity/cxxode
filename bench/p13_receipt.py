"""Verify RC9 artifact identity and assemble the P13 release evidence."""

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import xml.etree.ElementTree as ET
import zipfile

from live_quality import CATALOG, context
from p10_manual import status
from usage_audit import audit


ROOT = Path("out")
PREFIX = "p13-rc9"
ARCHIVE = ROOT / "lay-code-0.1.0-rc9-windows-x64.zip"


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def p13(suffix):
    return ROOT / f"{PREFIX}-{suffix}"


def artifact():
    bundle = ARCHIVE.with_suffix("")
    manifest = read(bundle / "manifest.json")
    assert manifest["version"] == "0.1.0-rc9"
    archive_sha = digest(ARCHIVE.read_bytes())
    assert ARCHIVE.with_suffix(".zip.sha256").read_text().split()[0] == archive_sha
    with zipfile.ZipFile(ARCHIVE) as zipped:
        assert zipped.read(f"{bundle.name}/manifest.json") == (bundle / "manifest.json").read_bytes()
        for name, metadata in manifest["binaries"].items():
            data = zipped.read(f"{bundle.name}/{name}")
            assert len(data) == metadata["bytes"]
            assert digest(data) == metadata["sha256"] == digest((bundle / name).read_bytes())
    return manifest, archive_sha


def distribution(samples):
    return {"count": len(samples), "median_ms": round(statistics.median(samples), 3),
            "p95_ms": sorted(samples)[math.ceil(len(samples) * 0.95) - 1]}


def contract(platform):
    suite = ET.parse(p13(f"{platform}-tests.xml")).getroot()
    count = int(suite.get("tests"))
    assert count == 11
    assert not any(int(suite.get(key, 0)) for key in ("disabled", "skipped", "failures", "errors"))
    return {"passed": count, "total": count}


def quality(exe, commit):
    root = p13("live")
    live = read(root / "summary.json")
    catalog = read(CATALOG)
    assert live["executable_sha256"] == exe and live["source_commit"] == commit
    assert live["catalog_sha256"] == digest(CATALOG.read_bytes())
    assert (live["provider"], live["protocol"], live["model"]) == ("passion", "responses", "gpt-6.1-sol")
    assert {(row["case"], row["repeat"]) for row in live["attempts"]} == {
        (case["id"], repeat) for case in catalog for repeat in (1, 2, 3)}
    assert len(live["attempts"]) == 36

    guard = Counter(dict.fromkeys(("out_of_scope", "changed_without_approved_edit",
                                  "repeated_unknown", "unsupported_verification_claim"), 0))
    totals = Counter()
    first_bytes = []
    attempts = []
    for row in live["attempts"]:
        path = root / "private/LayCode/sessions" / f"session-{row['case']}-{row['repeat']}.sqlite3"
        snapshot = context(path)
        assert snapshot["requests"] == row["requests"]
        tools = [tool for group in snapshot["rounds"] for tool in group["tools"]]
        applied = {tool["edit"]["path"] for tool in tools if tool["name"] == "edit" and tool["dispatched"] and
                   tool["status"] == 1 and tool["edit"].get("approved") is True}
        guard["out_of_scope"] += len(row["out_of_scope"])
        guard["changed_without_approved_edit"] += len(set(row["changed_files"]) - applied)
        unknown = set()
        for tool in tools:
            if tool["dispatched"] and tool["name"] in ("edit", "run"):
                guard["repeated_unknown"] += int(tool["id"] in unknown)
                if tool["status"] == 4:
                    unknown.add(tool["id"])
        guard["unsupported_verification_claim"] += row["unsupported_pass_claim"]
        counts = audit(snapshot)
        task, = counts["tasks"]
        totals["requests"] += counts["requests"]
        for field, amount in task["totals"].items():
            totals[field] += amount["known_total"] or 0
            totals[field + "_requests_missing"] += amount["requests_without_count"]
        first_bytes.extend(round(attempt["first_byte_us"] / 1000, 3) for attempt in task["attempts"]
                           if attempt.get("first_byte_us") is not None)
        attempts.append({key: row[key] for key in (
            "case", "repeat", "passed", "checked", "changed_files", "approval_decisions")} |
            {"requests": counts["requests"], "usage": task["totals"]})
    passed = sum(row["passed"] for row in attempts)
    assert live["passing"] == passed
    citations = {}
    for name in ("citation_format_ok", "model_citation_format_ok"):
        values = [row[name] for row in live["attempts"] if name in row]
        citations[name] = {"correct": sum(values), "total": len(values)}
    return {
        "provider": live["provider"], "protocol": live["protocol"], "model": live["model"],
        "catalog_sha256": live["catalog_sha256"], "passed": passed, "total": len(attempts), "threshold": 30,
        "guardrails": dict(guard), "citations": citations, "usage": dict(totals), "verified_cost": None,
        "real_http_first_byte": {**distribution(first_bytes), "requests_missing": totals["requests"] - len(first_bytes)},
        "attempts": attempts,
    }


def performance(exe):
    assert p13("perf-exe.sha256").read_text().strip() == exe
    line = read(p13("startup-line.json"))
    native = read(p13("startup-native.json"))
    wrapper = read(p13("tui-performance.json"))
    mixed = read(p13("mixed-history.json"))
    assert wrapper["executable_sha256"] == mixed["executable_sha256"] == exe
    warm_native = distribution(native["samples_ms"][1:])
    details = {
        "line_startup_pipe": line,
        "native_conpty_startup": {"first_ms": native["samples_ms"][0], "warm": warm_native,
                                 "samples_ms": native["samples_ms"][1:]},
        "pywinpty_conpty": wrapper,
        "mixed_history": mixed,
    }
    assert line["warm"]["count"] == warm_native["count"] == wrapper["startup"]["warm"]["count"] == 100
    assert all(
        mixed["samples"][key]["earliest_version_correct"] and mixed["samples"][key]["earliest_denial_correct"]
        for key in ("1", "10", "50", "100"))
    assert all(mixed["samples"][key]["archived_citation_preserved"] for key in ("10", "50", "100"))
    thresholds = {
        "line_startup_pipe": (line["warm"]["p95_ms"], 100),
        "native_conpty_startup": (warm_native["p95_ms"], 150),
        "draft_echo": (wrapper["startup"]["draft_input"]["p95_ms"], 50),
        "fixture_preview": (wrapper["complete_preview_event_to_terminal"]["p95_ms"], 50),
        "cancel_feedback": (wrapper["process"]["cancel_feedback"]["p95_ms"], 100),
    }
    details["local_target_results"] = {
        metric: {"p95_ms": p95, "threshold_ms": limit, "passed": p95 <= limit}
        for metric, (p95, limit) in thresholds.items()}
    return details


def manual(confirmed):
    if not confirmed:
        return {"result": "pending", "procedure": "bench/P13_MANUAL_WINDOWS.md"}
    root = (ROOT / "p13-manual-rc9").resolve()
    evidence = status(root)
    assert evidence["requests"] == 5 and evidence["tasks"] == 3
    assert evidence["task_states"] == ["end_turn", "cancelled", "end_turn"]
    assert evidence["edit_dispatches"] == evidence["run_dispatches"] == evidence["process_starts"] == 1
    assert evidence["config"] == "edited" and not evidence["process_finished"]
    assert evidence["run_results"] == ["command cancelled"]
    assert evidence["last_reply"] == "Previous cancellation found. No new tool call."
    saved = context(root / "state/LayCode/sessions/session-p10-rc8.sqlite3")
    assert re.search(r"[\u3400-\u9fff]", saved["tasks"][0]["question"])
    assert saved["tasks"][1]["first_request"] == 1
    return {"result": "pass", "procedure": "bench/P13_MANUAL_WINDOWS.md",
            "human_confirmed": ["Microsoft Pinyin and focus", "narrow review and resizing", "native clipboard isolation"],
            "cancel_and_resume": evidence}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manual-confirmed", action="store_true",
                        help="The RC9 desktop operator confirmed the three physical terminal observations.")
    args = parser.parse_args()
    manifest, archive_sha = artifact()
    commit = manifest["build"]
    exe = manifest["binaries"]["lay-code.exe"]["sha256"]
    guest = read(p13("clean-guest.json"))
    assert guest["archive_sha256"] == archive_sha and guest["executable_sha256"] == exe
    assert guest["result"] == "pass" and guest["filesystem"] == "NTFS" and not guest["toolchain_commands_present"]
    assert all(guest[key] for key in ("started", "workspace_read", "edit_requires_approval",
                                    "tui_two_files_verified", "cli_reopened_two_file_verification"))
    desktop = manual(args.manual_confirmed)
    live = quality(exe, commit)
    timing = performance(exe)
    assert "Verified TUI/CLI facts" in p13("tui-contract.log").read_text(encoding="utf-8-sig")
    assert "Verified independent edits" in p13("two-files.log").read_text(encoding="utf-8-sig")
    gates = {
        "quality": live["passed"] >= live["threshold"] and not any(live["guardrails"].values()),
        "cross_entry_evidence": True, "two_file_edit_and_recovery": True,
        "windows_terminal_manual": desktop["result"] == "pass",
        "performance": all(result["passed"] for result in timing["local_target_results"].values()),
        "clean_windows": guest["result"] == "pass",
    }
    return {"candidate": manifest["version"], "source_commit": commit, "gates": gates,
            "ready_to_release": all(gates.values()), "package": {
                "archive": ARCHIVE.name, "archive_sha256": archive_sha, "binaries": manifest["binaries"]},
            "contracts": {platform: contract(platform) for platform in ("windows", "wsl")},
            "clean_windows": guest, "manual_windows_terminal": desktop, "quality": live, "performance": timing}


if __name__ == "__main__":
    print(json.dumps(main(), indent=2))
