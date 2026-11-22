#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""Audit artifact wiring across v3 steps.

Catches the class of bug where an artifact is emitted by a step but
never consumed (vestigial / dead write), or read by a step that no other
step writes (orphan / typo). Either case is a smell — an emit-without-
consume often means the step was ported speculatively; a read-without-
emit means a name typo / missing pipeline step.

Usage:  python3 scripts/audit-artifact-wiring.py [--strict]

  --strict  exit non-zero if any orphan or write-only artifact is found
            (intended for CI gating).

Output sections:
  WRITE-ONLY  artifacts emitted by some step but never read by any.
  READ-ONLY   artifacts read by some step but never written by any.
  WRITERS     for each artifact, list the steps that emit it.

Limitations:
  - Parses C source via regex on `static const step_read_t … = { … }`
    and the matching writes table. Adequate for the current style
    (one table per step file); will miss anything generated at runtime.
  - .doc.json files are not consulted — only the C is the source of truth.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from collections import defaultdict


STEPS_ROOT = Path(__file__).resolve().parent.parent / "steps"

# Match     .name = t("artifact-name"),  inside reads[]/writes[].
NAME_RE = re.compile(r'\.name\s*=\s*t\(\s*"([^"]+)"\s*\)')

# Match the table header up to the opening `{`; the closing `};` is found
# via brace-counting because each record is `{ .name = t("…"), ... }`,
# which a `[^}]*?` body regex can't span.
HEAD_RE = re.compile(
    r'static\s+const\s+step_(read|write)_t\s+\w+\s*\[\s*\]\s*=\s*\{',
)


def parse_step_file(path: Path):
    """Return (reads, writes) — each a list of artifact names — for a step .c file."""
    txt = path.read_text(encoding="utf-8", errors="replace")
    reads, writes = [], []
    for m in HEAD_RE.finditer(txt):
        kind = m.group(1)
        depth = 1
        i = m.end()
        while i < len(txt) and depth > 0:
            ch = txt[i]
            if ch == '{':   depth += 1
            elif ch == '}': depth -= 1
            i += 1
        body = txt[m.end():i - 1]
        names = NAME_RE.findall(body)
        (reads if kind == "read" else writes).extend(names)
    return reads, writes


def find_step_files():
    return sorted(STEPS_ROOT.rglob("*.c"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--strict", action="store_true", help="non-zero exit if anything dangling")
    args = ap.parse_args()

    writers = defaultdict(list)   # artifact → [steps emitting]
    readers = defaultdict(list)   # artifact → [steps reading]
    per_step_reads  = {}
    per_step_writes = {}

    for f in find_step_files():
        # Step name = filename stem; safe heuristic for v3.
        step = f.stem
        reads, writes = parse_step_file(f)
        if not reads and not writes:
            continue
        per_step_reads[step]  = reads
        per_step_writes[step] = writes
        for a in reads:  readers[a].append(step)
        for a in writes: writers[a].append(step)

    # ── Write-only: emitted but nobody reads ───────────────────────────
    write_only = sorted(a for a in writers if a not in readers)
    # ── Read-only: read but nobody writes (pipeline-input is OK so we
    #    treat any artifact only-read by steps as suspicious unless it
    #    looks like a top-level input — lidar-positions, classifications,
    #    point-source-ids, etc.).
    read_only  = sorted(a for a in readers if a not in writers)

    KNOWN_INPUTS = {
        "lidar-positions", "classifications", "point-source-ids",
        "scan-angles", "return-numbers", "number-of-returns",
        "footprint", "ground-truth", "lidar-metadata",
    }
    read_only_suspicious = [a for a in read_only if a not in KNOWN_INPUTS]

    # ── Multi-writer artifacts: same name emitted by more than one step.
    multi_writers = {a: ws for a, ws in writers.items() if len(ws) > 1}

    # Output.
    print("=" * 72)
    print("v3 artifact-wiring audit")
    print("=" * 72)
    print(f"steps scanned: {len(per_step_writes)}    "
          f"artifacts written: {len(writers)}    read: {len(readers)}")
    print()

    print(f"WRITE-ONLY (emitted, never read) — {len(write_only)}")
    if write_only:
        for a in write_only:
            ws = ", ".join(writers[a])
            print(f"  {a:40s}  ← {ws}")
    else:
        print("  (none)")
    print()

    print(f"READ-ONLY suspicious (read, never written; not in KNOWN_INPUTS) — {len(read_only_suspicious)}")
    if read_only_suspicious:
        for a in read_only_suspicious:
            rs = ", ".join(readers[a])
            print(f"  {a:40s}  → {rs}")
    else:
        print("  (none)")
    print()

    if multi_writers:
        print(f"MULTI-WRITER (same name emitted by multiple steps) — {len(multi_writers)}")
        for a, ws in sorted(multi_writers.items()):
            print(f"  {a:40s}  ← {', '.join(ws)}")
        print()

    if args.strict and (write_only or read_only_suspicious):
        print("strict mode: dangling wiring found, exiting non-zero", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
