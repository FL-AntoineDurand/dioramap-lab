#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Code-duplication detection for the lab-ui front-end JavaScript, using jscpd.
#
# Scope:   apps/lab-ui/ ONLY (the JS surface we maintain).
#          node_modules/ is EXCLUDED.
#          lidar-algos-bench-v2/ is deliberately NOT scanned — that codebase
#          is being frozen once the port completes, so duplication there is
#          noise, not signal.
# Engine:  jscpd, restricted to the `javascript` format (lab-ui also carries
#          a little CSS/HTML/JSON that we don't want in the C-vs-JS split).
# Tuning:  --min-tokens 50 --min-lines 30 — comparable granularity to the
#          C-side CPD threshold.
#
# REPORT-ONLY / NON-GATING: always exits 0. Produces a JUnit XML (one
# testcase per clone cluster, all marked skip so nothing fails the tier)
# plus jscpd's own JSON report + console report under the report dir.
# Automated enforcement of the "one concept, N encodings" heuristic in
# docs/quality/REVIEW-HEURISTICS.md — surfaced for review, not a merge gate.
#
# Usage: run-jscpd.sh <out.xml> [report-dir]
#   <out.xml>     JUnit XML to write.
#   [report-dir]  where jscpd's JSON report lands.
#                 Defaults to <dirname out.xml>/jscpd-report.
#
# Tool install:
#   Tries `jscpd` on PATH first, else `npx --yes jscpd` (needs node + npm).
#   For a system install:  npm install -g jscpd
#   If neither jscpd nor npx is available, the step DEGRADES GRACEFULLY:
#   emits a single skipped testcase, prints an install hint, and exits 0.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"
. "$REPO_ROOT/scripts/ci/lib/junit.sh"

OUT="${1:?usage: run-jscpd.sh <out.xml> [report-dir]}"
REPORT_DIR="${2:-$(dirname "$OUT")/jscpd-report}"
mkdir -p "$(dirname "$OUT")" "$REPORT_DIR"

MIN_TOKENS=50
MIN_LINES=30
SCAN_DIR="apps/lab-ui"
JSCPD_VERSION="4.0.5"   # pinned for npx fallback reproducibility

junit_begin "jscpd-js"

if [[ ! -d "$SCAN_DIR" ]]; then
    msg="$SCAN_DIR not present; nothing to scan. Skipping (non-gating)."
    echo "[jscpd] $msg" >&2
    junit_record "jscpd-js-duplication" skip 0 "" "$msg"
    junit_end "$OUT"
    echo "[jscpd] → $OUT (skipped — no lab-ui)"
    exit 0
fi

# ── locate a jscpd entry point ─────────────────────────────────────────
JSCPD_CMD=()
if command -v jscpd >/dev/null 2>&1; then
    JSCPD_CMD=(jscpd)
elif command -v npx >/dev/null 2>&1; then
    JSCPD_CMD=(npx --yes "jscpd@${JSCPD_VERSION}")
else
    msg="jscpd not found and npx unavailable. Install with: npm install -g jscpd. Skipping (non-gating)."
    echo "[jscpd] $msg" >&2
    junit_record "jscpd-js-duplication" skip 0 "" "$msg"
    junit_end "$OUT"
    echo "[jscpd] → $OUT (skipped — tool absent)"
    exit 0
fi

# ── run jscpd ──────────────────────────────────────────────────────────
CONSOLE_LOG="$REPORT_DIR/jscpd.console.log"
JSON_REPORT="$REPORT_DIR/jscpd-report.json"

echo "[jscpd] lab-ui — min-tokens=$MIN_TOKENS min-lines=$MIN_LINES format=javascript (exclude node_modules)"

start_ns=$(date +%s%N)
"${JSCPD_CMD[@]}" "$SCAN_DIR" \
    --min-tokens "$MIN_TOKENS" \
    --min-lines "$MIN_LINES" \
    --format javascript \
    --ignore "**/node_modules/**" \
    --reporters json \
    --output "$REPORT_DIR" \
    --silent \
    >"$CONSOLE_LOG" 2>&1
jscpd_rc=$?
end_ns=$(date +%s%N)
duration=$(awk "BEGIN{print ($end_ns - $start_ns) / 1000000000}")

if [[ ! -f "$JSON_REPORT" ]]; then
    msg="jscpd produced no JSON report (rc=$jscpd_rc). $(tail -2 "$CONSOLE_LOG" 2>/dev/null | tr '\n' ' ')"
    echo "[jscpd] $msg" >&2
    junit_record "jscpd-js-duplication" skip "$duration" "$CONSOLE_LOG" "$msg"
    junit_end "$OUT"
    echo "[jscpd] → $OUT (skipped — jscpd error)"
    exit 0
fi

# ── parse the JSON report with node (already a hard dep of jscpd) ───────
# Emits TSV lines for each clone cluster, then a SUMMARY line.
parsed="$REPORT_DIR/jscpd.parsed.tsv"
node -e '
    const r = require(process.argv[1]);
    const t = (r.statistics && r.statistics.total) || {};
    const dups = r.duplicates || [];
    const base = p => (p || "").split("/").pop();
    let i = 0;
    for (const d of dups) {
        i++;
        const a = d.firstFile || {}, b = d.secondFile || {};
        const sa = (a.start != null) ? a.start : (a.startLoc && a.startLoc.line);
        const sb = (b.start != null) ? b.start : (b.startLoc && b.startLoc.line);
        const loc = `${base(a.name)}:${sa} <-> ${base(b.name)}:${sb}`;
        process.stdout.write([i, d.lines||0, d.tokens||0, 2, loc].join("\t") + "\n");
    }
    process.stderr.write(["SUMMARY", t.clones||0, t.duplicatedLines||0,
                          t.duplicatedTokens||0, t.percentage||0,
                          t.sources||0].join("\t") + "\n");
' "$JSON_REPORT" >"$parsed" 2>"$REPORT_DIR/jscpd.summary.tmp"

read -r _tag n_clusters dup_lines dup_tokens pct n_sources < "$REPORT_DIR/jscpd.summary.tmp"
n_clusters=${n_clusters:-0}; dup_lines=${dup_lines:-0}; dup_tokens=${dup_tokens:-0}
pct=${pct:-0}; n_sources=${n_sources:-0}
rm -f "$REPORT_DIR/jscpd.summary.tmp"

# Per-cluster JUnit testcases (skip → report-only). The detail goes into a
# per-cluster file so junit.sh renders it as <system-out> (skip testcases
# drop the failure-message arg).
detail_dir="$REPORT_DIR/clusters"
mkdir -p "$detail_dir"
while IFS=$'\t' read -r idx lines tokens occ loc; do
    [[ -z "$idx" ]] && continue
    name="clone-$(printf '%03d' "$idx")"
    detail_file="$detail_dir/$name.txt"
    printf '%s lines / %s tokens across %s sites:\n  %s\n' \
        "$lines" "$tokens" "$occ" "$loc" >"$detail_file"
    junit_record "$name" skip 0 "$detail_file"
done < "$parsed"

# Roll-up testcase — always present.
rollup_msg="clusters=${n_clusters} duplicated_lines=${dup_lines} duplicated_tokens=${dup_tokens} pct=${pct}% sources=${n_sources} (min-tokens=${MIN_TOKENS}, min-lines=${MIN_LINES})"
junit_record "jscpd-js-summary" skip "$duration" "$CONSOLE_LOG" "$rollup_msg"

junit_end "$OUT"

# ── human-readable summary ─────────────────────────────────────────────
echo "[jscpd] clusters=$n_clusters  duplicated_lines=$dup_lines  duplicated_tokens=$dup_tokens  pct=${pct}%  (over $n_sources JS files)"
if [[ "$n_clusters" -gt 0 ]]; then
    echo "[jscpd] top offenders (by token count):"
    sort -t$'\t' -k3,3 -rn "$parsed" 2>/dev/null | head -10 | \
        while IFS=$'\t' read -r idx lines tokens occ loc; do
            printf '[jscpd]   %4st / %3sl  %s\n' "$tokens" "$lines" "$loc"
        done
fi
echo "[jscpd] report: ${JSON_REPORT#$REPO_ROOT/}"
echo "[jscpd] → $OUT  (report-only, non-gating)"
exit 0
