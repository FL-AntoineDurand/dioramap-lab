#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Run libFuzzer targets and emit JUnit XML.
#
# Usage: run-fuzz.sh <build-dir> <out.xml> [<mode>] [<max-time-sec>]
#
#   build-dir      The fuzz build dir (e.g. core/build-fuzz). Must contain
#                  tests/fuzz/fuzz_<name> binaries.
#   out.xml        JUnit output path.
#   mode           "regression" (default) — replay the checked-in corpus
#                  once via `-runs=0` and exit; gating; takes seconds.
#                  "explore" — `-max_total_time=$MAX_TIME` per target;
#                  non-gating; intended for the nightly tier (task #85).
#   max-time-sec   Only used in explore mode; default 3600 (= 1 h each).
#
# Per docs/quality/README.md "Fuzz targets" — the corpus lives in
# core/tests/fuzz/corpus/<target>/ and is the canonical regression net.
# A crashing input is saved by libFuzzer as crash-<sha256> next to the
# binary; we surface that in the JUnit failure message but do NOT copy
# it into the artifact tree (intentionally — crashes need triage, not
# auto-archiving, and they may contain raw test bytes that look like
# garbage).

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"
. "$REPO_ROOT/scripts/ci/lib/junit.sh"

BUILD_DIR="${1:?usage: run-fuzz.sh <build-dir> <out.xml> [mode] [max-time]}"
OUT="${2:?usage: run-fuzz.sh <build-dir> <out.xml> [mode] [max-time]}"
MODE="${3:-regression}"
MAX_TIME="${4:-3600}"

FUZZ_DIR="$BUILD_DIR/tests/fuzz"
CORPUS_ROOT="$REPO_ROOT/core/tests/fuzz/corpus"
LOG_DIR="$(dirname "$OUT")"
mkdir -p "$LOG_DIR"

case "$MODE" in
    regression) ARGS=("-runs=0") ;;  # replay corpus, no new exploration
    explore)    ARGS=("-max_total_time=$MAX_TIME") ;;
    *)          echo "[fuzz] unknown mode: $MODE (try: regression | explore)" >&2; exit 2 ;;
esac

junit_begin "fuzz"
overall_rc=0

TARGETS=()
if [[ -d "$FUZZ_DIR" ]]; then
    for f in "$FUZZ_DIR"/fuzz_*; do
        [[ -x "$f" && -f "$f" ]] || continue
        TARGETS+=("$(basename "$f")")
    done
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    echo "[fuzz] no targets in $FUZZ_DIR — did you build with DIORAMAP_FUZZ=ON?" >&2
    junit_end "$OUT"
    exit 2
fi

echo "[fuzz] mode=$MODE targets=${#TARGETS[@]}"
for t in "${TARGETS[@]}"; do
    corpus="$CORPUS_ROOT/$t"
    log_file="$LOG_DIR/fuzz-${t}.log"
    if [[ ! -d "$corpus" ]]; then
        echo "[fuzz] $t :: SKIP  (no corpus dir at $corpus)"
        continue
    fi
    n_seeds=$(find "$corpus" -maxdepth 1 -type f | wc -l)

    start_ns=$(date +%s%N)
    set +e
    "$FUZZ_DIR/$t" "$corpus" "${ARGS[@]}" >"$log_file" 2>&1
    rc=$?
    set -e
    end_ns=$(date +%s%N)
    duration=$(awk "BEGIN{printf \"%.3f\", ($end_ns - $start_ns) / 1000000000}")

    if [[ "$rc" -eq 0 ]]; then
        echo "[fuzz] $t :: PASS  (${duration}s, corpus=$n_seeds, mode=$MODE)"
        junit_record "$t" pass "$duration" "$log_file"
    else
        # libFuzzer crash output: look for the canonical SUMMARY line or
        # the "Test unit written" hint that points at the crashing input.
        fmsg=$(grep -m1 -E 'SUMMARY:|ERROR:|Test unit written' "$log_file" \
               || echo "rc=$rc (see $log_file)")
        echo "[fuzz] $t :: FAIL  (${duration}s, rc=$rc) — $fmsg"
        junit_record "$t" fail "$duration" "$log_file" "$fmsg"
        overall_rc=1
    fi
done

junit_end "$OUT"
echo "[fuzz] → $OUT"
exit "$overall_rc"
