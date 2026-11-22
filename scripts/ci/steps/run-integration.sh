#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Run a set of integration scenarios against a binary, emit JUnit XML.
#
# Usage:
#   run-integration.sh <bin> <out.xml> [<variant-name>] [<scenarios...>]
#
# If <scenarios> is empty, runs every script under core/tests/integration/scenarios/.
# Otherwise runs the listed names (bare, without .sh).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$REPO_ROOT/scripts/ci/lib/junit.sh"

BIN="${1:?usage: run-integration.sh <bin> <out.xml> [variant-name] [scenarios...]}"
OUT="${2:?usage: run-integration.sh <bin> <out.xml> [variant-name] [scenarios...]}"
VARIANT="${3:-integration}"
shift 3 2>/dev/null || shift 2

SCEN_DIR="$REPO_ROOT/core/tests/integration/scenarios"
ORCH="$REPO_ROOT/core/tests/integration/run_scenario.sh"

# Integration scenarios shell out to node-ws.js for the WebSocket client
# side, which depends on the `ws` npm package. On a fresh clone (e.g. CI)
# node_modules doesn't exist; install on demand so the script is
# self-sufficient. `npm ci` when a lockfile is present (deterministic),
# `npm install` otherwise. Idempotent: re-running while node_modules
# exists is a no-op except for a quick mtime check.
IT_DIR="$REPO_ROOT/core/tests/integration"
if [[ ! -d "$IT_DIR/node_modules" ]]; then
    echo "[integration] installing node deps in core/tests/integration/ ..."
    if [[ -f "$IT_DIR/package-lock.json" ]]; then
        (cd "$IT_DIR" && npm ci --no-audit --no-fund --silent)
    else
        (cd "$IT_DIR" && npm install --no-audit --no-fund --silent)
    fi
fi

# Resolve scenario list — explicit names or every *.sh in the dir.
if [[ $# -eq 0 ]]; then
    SCENARIOS=()
    for s in "$SCEN_DIR"/*.sh; do
        SCENARIOS+=("$(basename "$s" .sh)")
    done
else
    SCENARIOS=("$@")
fi

mkdir -p "$(dirname "$OUT")"
LOG_DIR="$(dirname "$OUT")/integration-logs-$VARIANT"
mkdir -p "$LOG_DIR"

junit_begin "integration-$VARIANT"

overall_rc=0
for name in "${SCENARIOS[@]}"; do
    script="$SCEN_DIR/$name.sh"
    if [[ ! -f "$script" ]]; then
        echo "[integration] $name :: SKIP (no script)" >&2
        junit_record "$name" skip 0 "" "scenario script not found"
        continue
    fi

    log="$LOG_DIR/$name.log"
    start_ns=$(date +%s%N)
    set +e
    bash "$ORCH" "$name" "$BIN" >"$log" 2>&1
    rc=$?
    set -e
    end_ns=$(date +%s%N)
    duration=$(awk "BEGIN{print ($end_ns - $start_ns) / 1000000000}")

    if [[ $rc -eq 0 ]]; then
        echo "[integration] $name :: PASS (${duration}s)"
        junit_record "$name" pass "$duration" "$log"
    else
        echo "[integration] $name :: FAIL (${duration}s, rc=$rc)"
        # Last few lines of the log become the failure message
        # for at-a-glance reading in CI; full stdout still in the CDATA.
        fmsg=$(tail -3 "$log" 2>/dev/null | tr '\n' ' ' | head -c 200)
        junit_record "$name" fail "$duration" "$log" "$fmsg"
        overall_rc=1
    fi
done

junit_end "$OUT"
echo "[integration] variant=$VARIANT → $OUT"
exit "$overall_rc"
