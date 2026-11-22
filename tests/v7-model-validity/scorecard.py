#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""
tests/v7-model-validity/scorecard.py — aggregate per-case JSON results.

Usage:
    scorecard.py <tier> <out.json> [<case-json>...]
"""
import json
import subprocess
import sys
from datetime import datetime, timezone


def _git(args):
    try:
        return subprocess.check_output(["git", *args], text=True).strip()
    except subprocess.CalledProcessError:
        return "unknown"


def main(argv):
    if len(argv) < 3:
        print("usage: scorecard.py <tier> <out.json> [<case-json>...]", file=sys.stderr)
        return 2
    tier = argv[1]
    out_path = argv[2]
    cases = [json.loads(s) for s in argv[3:] if s.strip()]
    summary = {
        "schema_version": "1.0",
        "tier": tier,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "git_sha": _git(["rev-parse", "HEAD"]),
        "git_branch": _git(["rev-parse", "--abbrev-ref", "HEAD"]),
        "case_count": len(cases),
        "summary": {
            "passed": sum(1 for c in cases if c.get("passed")),
            "failed": sum(1 for c in cases if not c.get("passed")),
        },
        "cases": {c.get("case_id", f"unknown-{i}"): c for i, c in enumerate(cases)},
    }
    with open(out_path, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"  passed={summary['summary']['passed']} failed={summary['summary']['failed']}  →  {out_path}")
    return 0 if summary["summary"]["failed"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
