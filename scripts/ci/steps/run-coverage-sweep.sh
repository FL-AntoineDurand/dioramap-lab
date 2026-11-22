#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Run ctest + the integration scenarios against a coverage-instrumented
# dioramap-processor, collecting .profraw files for later merging.
#
# Usage: run-coverage-sweep.sh <profraw-dir> <out.xml> [<integration-scenarios...>]
#
# - <profraw-dir>     where .profraw files land; LLVM_PROFILE_FILE is set to
#                     "<dir>/cov-%p-%m.profraw" so every process gets its
#                     own non-colliding file.
# - <out.xml>         JUnit XML for the coverage-tier test results
#                     (ctest + integration, both run under instrumentation).
# - <scenarios...>    optional; default = every scenario under
#                     core/tests/integration/scenarios/.
#
# The build dir is hard-coded to core/build-cov (per variants.sh).
# Caller is responsible for building cov first.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"
. "$REPO_ROOT/scripts/ci/lib/junit.sh"

PROFRAW_DIR="${1:?usage: run-coverage-sweep.sh <profraw-dir> <out.xml> [scenarios...]}"
OUT="${2:?usage: run-coverage-sweep.sh <profraw-dir> <out.xml> [scenarios...]}"
shift 2
SCENARIOS=("$@")

BUILD_DIR="core/build-cov"
BIN="$BUILD_DIR/dioramap-processor"

[[ -x "$BIN" ]] || { echo "[cov] $BIN not built — run build-variant.sh cov first" >&2; exit 2; }

mkdir -p "$PROFRAW_DIR"
LOG_DIR="$(dirname "$OUT")/cov-logs"
mkdir -p "$LOG_DIR"

# %p = pid, %m = hash of the binary's coverage instrumentation map.  The
# combination guarantees a unique file per (pid, binary) — important
# because the integration harness spawns multiple distinct processes.
export LLVM_PROFILE_FILE="$PROFRAW_DIR/cov-%p-%m.profraw"

# Coverage instrumentation is ~2× — tests like serve_full.py honour
# DIORAMAP_TEST_TIMEOUT_MULT so their wall-clock recv budgets scale
# accordingly (same env that run-ctest.sh sets on tsan/cov).
export DIORAMAP_TEST_TIMEOUT_MULT="${DIORAMAP_TEST_TIMEOUT_MULT:-2.0}"

junit_begin "coverage-sweep"
overall_rc=0

# ── ctest ─────────────────────────────────────────────────────────────
ctest_log="$LOG_DIR/ctest-cov.log"
set +e
ctest --test-dir "$BUILD_DIR" \
      --output-on-failure \
      -j "$(nproc)" \
      >"$ctest_log" 2>&1
ctest_rc=$?
set -e

# Count test names from the ctest log (one line per test).  Imperfect but
# good enough for the dashboard summary.
#
# `grep -c X || echo 0` was a footgun: when grep matches but the count is
# zero (rc=1, output "0"), the OR also fires and we end up with the
# two-line string "0\n0" which then trips $((... - ...)) with
# "arithmetic syntax error in expression (error token is "0")". awk is
# both clearer and always single-line / rc=0.
ctest_total=$(awk '/^[[:space:]]*[0-9]+\/[0-9]+ Test/{c++} END{print c+0}' "$ctest_log")
ctest_failed=$(awk '/\*\*\*Failed/{c++} END{print c+0}' "$ctest_log")
ctest_passed=$((ctest_total - ctest_failed))

if [[ "$ctest_rc" -eq 0 ]]; then
    echo "[cov] ctest :: PASS ($ctest_passed/$ctest_total)"
    junit_record "ctest-under-cov" pass 0 "$ctest_log"
else
    fmsg=$(grep -E 'tests failed out of' "$ctest_log" | tail -1)
    echo "[cov] ctest :: FAIL ($fmsg)"
    junit_record "ctest-under-cov" fail 0 "$ctest_log" "$fmsg"
    overall_rc=1
fi

# ── integration scenarios ─────────────────────────────────────────────
SCEN_DIR="$REPO_ROOT/core/tests/integration/scenarios"
ORCH="$REPO_ROOT/core/tests/integration/run_scenario.sh"

if [[ ${#SCENARIOS[@]} -eq 0 ]]; then
    SCENARIOS=()
    for s in "$SCEN_DIR"/*.sh; do
        SCENARIOS+=("$(basename "$s" .sh)")
    done
fi

for name in "${SCENARIOS[@]}"; do
    log="$LOG_DIR/$name.log"
    start_ns=$(date +%s%N)
    set +e
    bash "$ORCH" "$name" "$REPO_ROOT/$BIN" >"$log" 2>&1
    rc=$?
    set -e
    end_ns=$(date +%s%N)
    duration=$(awk "BEGIN{print ($end_ns - $start_ns) / 1000000000}")
    if [[ "$rc" -eq 0 ]]; then
        echo "[cov] integration $name :: PASS (${duration}s)"
        junit_record "integration-$name" pass "$duration" "$log"
    else
        fmsg=$(tail -3 "$log" 2>/dev/null | tr '\n' ' ' | head -c 200)
        echo "[cov] integration $name :: FAIL (${duration}s) — $fmsg"
        junit_record "integration-$name" fail "$duration" "$log" "$fmsg"
        overall_rc=1
    fi
done

junit_end "$OUT"

# Sanity: did anything get written?
n_profraw=$(find "$PROFRAW_DIR" -name '*.profraw' 2>/dev/null | wc -l)
echo "[cov] $n_profraw .profraw files in $PROFRAW_DIR"

echo "[cov] sweep → $OUT"
exit "$overall_rc"
