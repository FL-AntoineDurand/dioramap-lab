#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Merge .profraw → .profdata and emit a summary.json + (best-effort) HTML.
#
# Usage: run-coverage-report.sh <profraw-dir> <out-dir>
#
# Produces:
#   <out-dir>/all.profdata           merged profile  (always)
#   <out-dir>/profdata-show.txt      llvm-profdata show output (always)
#   <out-dir>/summary.json           machine-readable summary (always)
#   <out-dir>/coverage.txt           llvm-cov report (best-effort, may be empty)
#   <out-dir>/html/index.html        llvm-cov HTML report (best-effort, may be missing)
#
# Both `llvm-cov report` and `llvm-cov show` are wrapped in `timeout` —
# llvm-cov-18 in Ubuntu 24.04 hangs on dioramap-processor's full coverage
# map (~2.6 MB __llvm_prf_data) for reasons we haven't fully diagnosed.
# The bounded fallback keeps the tier from getting stuck.
#
# The summary.json is generated from `llvm-profdata show` which is fast
# and reliable; the dashboard reads it.  HTML drill-down is gravy.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"

PROFRAW_DIR="${1:?usage: run-coverage-report.sh <profraw-dir> <out-dir>}"
OUT_DIR="${2:?usage: run-coverage-report.sh <profraw-dir> <out-dir>}"

BIN="$REPO_ROOT/core/build-cov/dioramap-processor"

mkdir -p "$OUT_DIR" "$OUT_DIR/html"

# Tool locations.
#
# We MUST match the system clang's version (currently clang-21 on
# Ubuntu 26.04, was clang-18 on 24.04): clang stamps each .profraw with
# a version tag that only the matching llvm-profdata can read.
# `llvm-profdata-18 merge` on a 21-format profraw prints
# "PLEASE update this tool to version in the raw profile..." for every
# input and exits 1.
#
# Search 21 first (current target), then 18 (24.04 fallback), then
# unversioned. The historical comment about llvm-cov-18 hanging on
# dioramap-processor's coverage map is moot now — we're on 21 and the
# bigger problem is just being able to read the file at all.
LLVM_PROFDATA="$(command -v llvm-profdata-21 \
              || command -v llvm-profdata-18 \
              || command -v llvm-profdata)"
LLVM_COV="$(command -v llvm-cov-21 \
         || command -v llvm-cov-18 \
         || command -v llvm-cov)"
[[ -n "$LLVM_PROFDATA" ]] || { echo "[cov-report] need llvm-profdata on PATH" >&2; exit 2; }

# ── 1. Merge .profraw → .profdata ─────────────────────────────────────
n_profraw=$(find "$PROFRAW_DIR" -name '*.profraw' 2>/dev/null | wc -l)
echo "[cov-report] merging $n_profraw .profraw files"
if [[ "$n_profraw" -eq 0 ]]; then
    echo "[cov-report] no .profraw files — nothing to report" >&2
    exit 0
fi
"$LLVM_PROFDATA" merge -sparse "$PROFRAW_DIR"/*.profraw -o "$OUT_DIR/all.profdata"

# ── 2. profdata show (fast, never hangs) ──────────────────────────────
"$LLVM_PROFDATA" show "$OUT_DIR/all.profdata" > "$OUT_DIR/profdata-show.txt" 2>&1

# Extract basic numbers from `llvm-profdata show`.
total_funcs=$(grep -oE 'Total functions: [0-9]+' "$OUT_DIR/profdata-show.txt" | awk '{print $NF}')
max_count=$(grep -oE 'Maximum function count: [0-9]+' "$OUT_DIR/profdata-show.txt" | awk '{print $NF}')

# ── 3. llvm-cov report (best-effort, time-bounded) ────────────────────
COV_TIMEOUT="${COV_TIMEOUT:-120}"
report_status="ok"
if [[ -n "$LLVM_COV" && -x "$BIN" ]]; then
    echo "[cov-report] tabular report (timeout=${COV_TIMEOUT}s, may be skipped if llvm-cov hangs)"
    set +e
    timeout "$COV_TIMEOUT" "$LLVM_COV" report \
        --instr-profile="$OUT_DIR/all.profdata" \
        "$BIN" \
        "$REPO_ROOT/core/src" "$REPO_ROOT/steps" \
        </dev/null > "$OUT_DIR/coverage.txt" 2>&1
    rc=$?
    set -e
    case "$rc" in
        0)   report_status="ok" ;;
        124) report_status="timeout"; echo "[cov-report] tabular: timed out after ${COV_TIMEOUT}s — known issue with llvm-cov-18" ;;
        *)   report_status="error"; echo "[cov-report] tabular: llvm-cov returned $rc" ;;
    esac
else
    report_status="skipped"
fi

# Extract TOTAL line from the report (if any).
total_line=$(grep -E '^TOTAL' "$OUT_DIR/coverage.txt" 2>/dev/null | head -1 || true)
line_pct=""
region_pct=""
func_pct=""
branch_pct=""
if [[ -n "$total_line" ]]; then
    pcts=$(grep -oE '[0-9]+\.[0-9]+%' <<<"$total_line" || true)
    region_pct=$(awk 'NR==1' <<<"$pcts" | tr -d '%')
    func_pct=$(awk   'NR==2' <<<"$pcts" | tr -d '%')
    line_pct=$(awk   'NR==3' <<<"$pcts" | tr -d '%')
    branch_pct=$(awk 'NR==4' <<<"$pcts" | tr -d '%')
fi

# ── 4. llvm-cov show → HTML (best-effort) ─────────────────────────────
html_status="skipped"
if [[ "$report_status" == "ok" ]]; then
    echo "[cov-report] HTML report (timeout=$((COV_TIMEOUT * 2))s)"
    set +e
    timeout $((COV_TIMEOUT * 2)) "$LLVM_COV" show \
        --instr-profile="$OUT_DIR/all.profdata" \
        "$BIN" \
        --format=html \
        --output-dir="$OUT_DIR/html" \
        --show-line-counts-or-regions \
        --ignore-filename-regex='third_party|/usr/' \
        "$REPO_ROOT/core/src" "$REPO_ROOT/steps" \
        </dev/null >/dev/null 2>&1
    rc=$?
    set -e
    case "$rc" in
        0)   html_status="ok" ;;
        124) html_status="timeout" ;;
        *)   html_status="error" ;;
    esac
fi

# ── 5. Summary.json ───────────────────────────────────────────────────
{
    echo '{'
    echo '  "tool": "llvm-profdata-18 + llvm-cov-18",'
    echo "  \"n_profraw\": $n_profraw,"
    echo "  \"profdata_total_functions\": ${total_funcs:-0},"
    echo "  \"profdata_max_count\": ${max_count:-0},"
    echo "  \"report_status\": \"$report_status\","
    echo "  \"html_status\": \"$html_status\","
    if [[ -n "$line_pct" ]]; then
        echo '  "totals": {'
        echo "    \"region_pct\":   ${region_pct:-0},"
        echo "    \"function_pct\": ${func_pct:-0},"
        echo "    \"line_pct\":     ${line_pct:-0},"
        echo "    \"branch_pct\":   ${branch_pct:-0}"
        echo '  },'
    fi
    echo '  "files": {'
    echo "    \"profdata\":      \"all.profdata\","
    echo "    \"profdata_show\": \"profdata-show.txt\","
    echo "    \"tabular\":       \"coverage.txt\","
    echo "    \"html_index\":    \"html/index.html\""
    echo '  }'
    echo '}'
} > "$OUT_DIR/summary.json"

echo "[cov-report] done"
echo "  $OUT_DIR/summary.json"
echo "  report_status=$report_status html_status=$html_status"
if [[ -n "$line_pct" ]]; then
    echo "  line=${line_pct}%  function=${func_pct}%  region=${region_pct}%  branch=${branch_pct}%"
fi

# Always exit 0 — coverage is a measurement, not a gate.
exit 0
