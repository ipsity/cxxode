"""Recompute task usage from a local SQLite session without reading source or credentials."""

import argparse
from contextlib import closing
import json
from pathlib import Path
import sqlite3


def summarize(attempts, requests, field):
    counts = [row[field] for row in attempts if row.get(field) is not None]
    return {"known_total": sum(counts) if counts else None, "requests_without_count": requests - len(counts)}


def audit(snapshot):
    requests = snapshot["requests"]
    recorded = snapshot.get("model_attempts", [])
    complete_legacy_sequence = len(recorded) == requests and all(
        attempt.get("request_number", i) == i for i, attempt in enumerate(recorded, 1))
    by_request = {}
    unattributed = []
    for i, attempt in enumerate(recorded, 1):
        number = attempt.get("request_number", i if complete_legacy_sequence else None)
        row = {key: attempt.get(key) for key in ("body_bytes", "uploaded_body_bytes",
               "input_tokens", "output_tokens", "provider_usage", "first_byte_us")}
        if "attempt_state" in attempt:
            row["attempt_state"] = ("reserved", "no_receipt", "response")[attempt["attempt_state"]]
            row["http_status"] = attempt.get("http_status")
        if number is None:
            unattributed.append(row)
        elif not 1 <= number <= requests or number in by_request:
            raise ValueError("Invalid request number in model attempt")
        else:
            by_request[number] = row
    tasks = snapshot.get("tasks") or [{"id": 1, "first_request": 0}]
    output = []
    for i, task in enumerate(tasks):
        first = task["first_request"]
        end = tasks[i + 1]["first_request"] if i + 1 < len(tasks) else requests
        attempts = [{"request_number": number, **by_request.get(number, {})}
                    for number in range(first + 1, end + 1)]
        output.append({"task": task["id"], "requests": end - first,
                       "totals": {field: summarize(attempts, end - first, field) for field in
                                  ("body_bytes", "uploaded_body_bytes", "input_tokens", "output_tokens")},
                       "attempts": attempts})
    return {"requests": requests, "tasks": output, "unattributed_legacy_attempts": unattributed,
            "verified_cost": None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="Lay Code session SQLite file")
    args = parser.parse_args()
    uri = args.database.resolve(strict=True).as_uri() + "?mode=ro"
    with closing(sqlite3.connect(uri, uri=True)) as db:
        payload, = db.execute("SELECT snapshot FROM session WHERE id=1").fetchone()
    print(json.dumps(audit(json.loads(payload)), indent=2))


if __name__ == "__main__":
    main()
