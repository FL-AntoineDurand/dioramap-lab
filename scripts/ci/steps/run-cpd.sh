#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Copy-paste / code-duplication detection for the durable C surface, using
# PMD's CPD (Copy-Paste Detector).
#
# Scope:   steps/ + core/src/   (the C code we maintain).
#          third_party/ is EXCLUDED — upstream clones aren't ours to dedupe.
# Engine:  PMD CPD, token-based, language `cpp` (covers C and C++).
# Tuning:  --minimum-tokens 75 — flags the real near-duplicate decode loops
#          (e.g. the parse_corners / parse_edges / parse_polygon_paths
#          rg_record_iter + safe_calloc_array blocks copied across the v7
#          boundary step wrappers) without drowning in short boilerplate.
#
# GATING since task #72c — clusters above CPD_GATE_TOKENS (default 300)
# are reported as JUnit failures and the step exits non-zero. The roll-up
# remains a skip (informational); only the offending per-cluster
# testcases trip the gate. Override with `CPD_GATE_TOKENS=0` to revert to
# report-only locally.
#
# Threshold rationale: after the #72c family-A→G sweep, the top remaining
# cluster sat at 265 tokens (the v7 PLANES parser, 5-file pattern with 3
# distinct axis-swap variants — extracting cleanly would require a
# bitmask/callback, the "weird code splitting" we want to avoid). 300t
# lets that pattern stand while catching any regression to the
# pre-sweep state where Families A and D were 416-566 tokens.
#
# Usage: run-cpd.sh <out.xml> [report-dir]
#   <out.xml>     JUnit XML to write.
#   [report-dir]  where the raw CPD reports (cpd.csv, cpd.txt) land.
#                 Defaults to <dirname out.xml>/cpd-report.
#
# Tool install:
#   Tries, in order: `cpd` on PATH, `pmd cpd` on PATH, then a cached PMD
#   distribution under scripts/ci/.tools/pmd-bin-*/bin/pmd (gitignored —
#   download once with:
#     v=7.10.0
#     curl -sSL -o /tmp/pmd.zip \
#       "https://github.com/pmd/pmd/releases/download/pmd_releases%2F${v}/pmd-dist-${v}-bin.zip"
#     mkdir -p scripts/ci/.tools && unzip -q /tmp/pmd.zip -d scripts/ci/.tools
#   PMD needs a JRE (java 8+) on PATH.
#   If no CPD is found, the step DEGRADES GRACEFULLY: emits a single skipped
#   testcase, prints an install hint, and exits 0.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"
. "$REPO_ROOT/scripts/ci/lib/junit.sh"

OUT="${1:?usage: run-cpd.sh <out.xml> [report-dir]}"
REPORT_DIR="${2:-$(dirname "$OUT")/cpd-report}"
mkdir -p "$(dirname "$OUT")" "$REPORT_DIR"

MIN_TOKENS=75
# Any cluster with > GATE_TOKENS tokens is reported as a JUnit failure
# and trips the step's exit code. See header.
GATE_TOKENS="${CPD_GATE_TOKENS:-300}"
LANGUAGE=cpp
SCAN_DIRS=(steps core/src)
# steps/polyfit, steps/ksr, steps/roofer are thin wrappers around upstream
# C++ libs (PolyFit / CGAL-KSR / Roofer); the bulk-clusters CPD flags there
# are wrapper-boilerplate around the upstream API, not v3-owned algorithm
# duplication. Excluding so the v3 surface signal isn't drowned in research-
# shim noise. Task #72c.
EXCLUDE_DIRS=(third_party)

# ── locate a CPD entry point ───────────────────────────────────────────
# CPD_CMD is an array so we can carry `pmd cpd` (two words) uniformly.
CPD_CMD=()
if command -v cpd >/dev/null 2>&1; then
    CPD_CMD=(cpd)
elif command -v pmd >/dev/null 2>&1; then
    CPD_CMD=(pmd cpd)
else
    cached="$(ls -d "$REPO_ROOT"/scripts/ci/.tools/pmd-bin-*/bin/pmd 2>/dev/null | sort | tail -1)"
    if [[ -n "$cached" && -x "$cached" ]]; then
        CPD_CMD=("$cached" cpd)
    fi
fi

junit_begin "cpd-c"

if [[ ${#CPD_CMD[@]} -eq 0 ]]; then
    msg="PMD CPD not found (tried: cpd, pmd, scripts/ci/.tools/pmd-bin-*/bin/pmd). Install PMD — see script header. Skipping (non-gating)."
    echo "[cpd] $msg" >&2
    junit_record "cpd-c-duplication" skip 0 "" "$msg"
    junit_end "$OUT"
    echo "[cpd] → $OUT (skipped — tool absent)"
    exit 0
fi

if ! command -v java >/dev/null 2>&1; then
    msg="java not on PATH; PMD CPD needs a JRE (8+). Skipping (non-gating)."
    echo "[cpd] $msg" >&2
    junit_record "cpd-c-duplication" skip 0 "" "$msg"
    junit_end "$OUT"
    echo "[cpd] → $OUT (skipped — no JRE)"
    exit 0
fi

# ── run CPD ────────────────────────────────────────────────────────────
CSV="$REPORT_DIR/cpd.csv"
TXT="$REPORT_DIR/cpd.txt"
ERRLOG="$REPORT_DIR/cpd.stderr.log"

dir_args=()
for d in "${SCAN_DIRS[@]}"; do dir_args+=(--dir "$d"); done
exclude_args=()
for d in "${EXCLUDE_DIRS[@]}"; do exclude_args+=(--exclude "$d"); done

echo "[cpd] PMD CPD — lang=$LANGUAGE min-tokens=$MIN_TOKENS dirs=${SCAN_DIRS[*]} (exclude: ${EXCLUDE_DIRS[*]})"

start_ns=$(date +%s%N)
# --format csv_with_linecount_per_file would give per-file line counts but a
# different column layout; plain csv (lines,tokens,occurrences,<startline,file>...)
# is the easiest to total. We also keep a human-readable text report.
"${CPD_CMD[@]}" \
    --minimum-tokens "$MIN_TOKENS" \
    --language "$LANGUAGE" \
    --no-fail-on-violation \
    --no-fail-on-error \
    --format csv \
    "${dir_args[@]}" \
    "${exclude_args[@]}" \
    >"$CSV" 2>"$ERRLOG"
csv_rc=$?

# Also produce a human-readable text report (block-by-block).
"${CPD_CMD[@]}" \
    --minimum-tokens "$MIN_TOKENS" \
    --language "$LANGUAGE" \
    --no-fail-on-violation \
    --no-fail-on-error \
    --format text \
    "${dir_args[@]}" \
    "${exclude_args[@]}" \
    >"$TXT" 2>>"$ERRLOG"

end_ns=$(date +%s%N)
duration=$(awk "BEGIN{print ($end_ns - $start_ns) / 1000000000}")

# CPD exits non-zero only on hard errors (we pass --no-fail-on-*). Treat a
# non-zero exit WITHOUT a usable CSV as a tool error → skip, still exit 0.
if [[ "$csv_rc" -ne 0 && ! -s "$CSV" ]]; then
    msg="CPD failed to run (rc=$csv_rc). $(tail -2 "$ERRLOG" 2>/dev/null | tr '\n' ' ')"
    echo "[cpd] $msg" >&2
    junit_record "cpd-c-duplication" skip "$duration" "$ERRLOG" "$msg"
    junit_end "$OUT"
    echo "[cpd] → $OUT (skipped — CPD error)"
    exit 0
fi

# ── parse the CSV ──────────────────────────────────────────────────────
# Header:  lines,tokens,occurrences,<startline,file>,<startline,file>,...
# One data row per clone cluster. We total clusters, duplicated lines and
# tokens, and emit a per-cluster JUnit testcase (skip status → never fails).
#
# awk computes the aggregates and prints, for each cluster:
#   <idx>\t<lines>\t<tokens>\t<occurrences>\t<file1[:line1]>...
parsed="$REPORT_DIR/cpd.parsed.tsv"
awk -F, '
    NR == 1 { next }                       # skip header
    NF < 5  { next }                        # malformed/blank
    {
        lines = $1; tokens = $2; occ = $3;
        clusters++;
        total_lines  += lines;
        total_tokens += tokens;
        # collect the (startline,file) pairs into a compact location list
        loc = "";
        for (i = 4; i + 1 <= NF; i += 2) {
            f = $(i+1); ln = $i;
            n = split(f, parts, "/"); base = parts[n];
            loc = loc (loc == "" ? "" : " ") base ":" ln;
        }
        printf "%d\t%d\t%d\t%d\t%s\n", clusters, lines, tokens, occ, loc;
    }
    END {
        print "SUMMARY\t" clusters+0 "\t" total_lines+0 "\t" total_tokens+0 > "/dev/stderr";
    }
' "$CSV" >"$parsed" 2>"$REPORT_DIR/cpd.summary.tmp"

read -r _tag n_clusters dup_lines dup_tokens < "$REPORT_DIR/cpd.summary.tmp"
n_clusters=${n_clusters:-0}; dup_lines=${dup_lines:-0}; dup_tokens=${dup_tokens:-0}
rm -f "$REPORT_DIR/cpd.summary.tmp"

# Per-cluster JUnit testcases. Clusters above GATE_TOKENS are recorded
# as failures (and counted in gate_failures so we exit non-zero); the
# rest stay as skips so the dashboard sees them but they don't trip the
# gate.
detail_dir="$REPORT_DIR/clusters"
mkdir -p "$detail_dir"
gate_failures=0
while IFS=$'\t' read -r idx lines tokens occ loc; do
    [[ -z "$idx" ]] && continue
    name="clone-$(printf '%03d' "$idx")"
    detail_file="$detail_dir/$name.txt"
    printf '%s lines / %s tokens across %s sites:\n  %s\n' \
        "$lines" "$tokens" "$occ" "$loc" >"$detail_file"
    if (( GATE_TOKENS > 0 && tokens > GATE_TOKENS )); then
        gate_failures=$((gate_failures + 1))
        fmsg="duplicate cluster ${tokens}t > gate ${GATE_TOKENS}t across ${occ} sites"
        junit_record "$name" fail 0 "$detail_file" "$fmsg"
    else
        junit_record "$name" skip 0 "$detail_file"
    fi
done < "$parsed"

# A roll-up testcase carrying the headline numbers as its message — always
# present even when there are zero clones, so the dashboard has a row.
rollup_msg="clusters=${n_clusters} duplicated_lines=${dup_lines} duplicated_tokens=${dup_tokens} (min-tokens=${MIN_TOKENS}, lang=${LANGUAGE})"
junit_record "cpd-c-summary" skip "$duration" "$TXT" "$rollup_msg"

junit_end "$OUT"

# ── human-readable summary ─────────────────────────────────────────────
echo "[cpd] clusters=$n_clusters  duplicated_lines=$dup_lines  duplicated_tokens=$dup_tokens"
echo "[cpd] top offenders (by token count):"
# Sort parsed rows by tokens desc, show top 10.
sort -t$'\t' -k3,3 -rn "$parsed" 2>/dev/null | head -10 | \
    while IFS=$'\t' read -r idx lines tokens occ loc; do
        printf '[cpd]   %4st / %3sl  ×%s  %s\n' "$tokens" "$lines" "$occ" "$loc"
    done
echo "[cpd] reports: ${CSV#$REPO_ROOT/} , ${TXT#$REPO_ROOT/}"
if (( GATE_TOKENS > 0 && gate_failures > 0 )); then
    echo "[cpd] → $OUT  (gate ${GATE_TOKENS}t: ${gate_failures} cluster(s) over threshold)"
    exit 1
fi
echo "[cpd] → $OUT  (gate ${GATE_TOKENS}t: all clusters within threshold)"
exit 0
