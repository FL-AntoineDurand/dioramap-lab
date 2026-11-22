#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# batch_demo_chain_warm
#
# Bug class: cache-hit class (cb_on_artifact_ready callbacks fire in
# parallel from worker threads on cache-hit runs — see
# BUG-cache-hit-runner-cleanup.md).  Smallest reproducer that exercises
# the journal-acc path under batch mode.
#
# Approach:
#   1. Run dioramap-processor batch on demo-chain.json with input {"base":5}
#      → first run is all cache misses, populates the cache.
#   2. Run again, identical input + cache dir
#      → second run should be all cache hits (steps_executed:0).
# Asserts:
#   - both runs exit 0
#   - first run's manifest.json shows ok:true
#   - second run's manifest.json shows ok:true
#   - no sanitizer reports in either run's stderr
#
# Variants this matters most for: asan (heap correctness on cache-hit
# path), tsan (when matrix lands; cache hits are the deterministic
# race repro).

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "batch_demo_chain_warm"
require "$SCEN_BIN" jq

PIPELINE="$SCEN_PIPELINES_DIR/smoke/demo-chain.json"
INPUT='{"base":5}'

run_once() {
    local label="$1"
    local out_dir="$SCEN_OUTPUT_DIR/$label"
    mkdir -p "$out_dir"
    local log="$SCEN_TMPDIR/${label}.log"
    if ! "$SCEN_BIN" batch \
            --pipeline "$PIPELINE" \
            --input-json "$INPUT" \
            --produce report \
            --output-dir "$out_dir" \
            --cache-dir "$SCEN_CACHE_DIR" \
            >"$log" 2>&1; then
        echo "[$SCEN_NAME] $label run failed; log:" >&2
        tail -20 "$log" >&2
        fail "batch $label run exit non-zero"
    fi
    if grep -qE 'AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer' "$log"; then
        echo "[$SCEN_NAME] sanitizer report in $log:" >&2
        grep -A5 -E 'Sanitizer' "$log" | head -30 >&2
        fail "$label sanitizer report"
    fi
    local manifest="$out_dir/manifest.json"
    [[ -f "$manifest" ]] || fail "$label manifest.json not written"
    local ok
    ok=$(jq -r '.ok' "$manifest")
    [[ "$ok" == "true" ]] || fail "$label manifest ok != true (got: $ok)"
    # Parse the structured run_complete log line — fields:
    #   "steps_executed":N "cache_hits":M
    # Print "<executed> <hits>" so caller can split.
    local line
    line=$(grep '"event":"run_complete"' "$log" | tail -1)
    [[ -n "$line" ]] || fail "$label no run_complete log line"
    local executed hits
    executed=$(jq -r '.steps_executed' <<<"$line")
    hits=$(jq -r '.cache_hits' <<<"$line")
    echo "$executed $hits"
}

echo "[$SCEN_NAME] cold run..."
read -r COLD_EXEC COLD_HITS < <(run_once cold)
echo "[$SCEN_NAME] cold: executed=$COLD_EXEC hits=$COLD_HITS"

echo "[$SCEN_NAME] warm run..."
read -r WARM_EXEC WARM_HITS < <(run_once warm)
echo "[$SCEN_NAME] warm: executed=$WARM_EXEC hits=$WARM_HITS"

# demo-chain has 4 steps.  Cold run: 4 executed, 0 hits.  Warm: 0 / 4.
[[ "$COLD_EXEC" == "4" && "$COLD_HITS" == "0" ]] \
    || fail "cold expected 4/0, got $COLD_EXEC/$COLD_HITS"
[[ "$WARM_EXEC" == "0" && "$WARM_HITS" == "4" ]] \
    || fail "warm expected 0/4, got $WARM_EXEC/$WARM_HITS"

pass "cold=4/0 warm=0/4 as expected"
