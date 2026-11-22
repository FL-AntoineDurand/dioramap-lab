#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# serve_all_cache_hit_burst
#
# Bug class: BUG-cache-hit-runner-cleanup.md (the journal-acc race).
#
# Approach: drive the processor with N identical RUN messages on one
# socket (sequential; uses node client's `burst` command).  After the
# first RUN, every subsequent run is all-cache-hit — every step's
# cb_on_artifact_ready callback fires from a worker thread in
# microseconds.  Without the encapsulation fix (or with a future
# regression), worker threads race on the journal accumulator and
# either (a) ASan/TSan catches it, (b) the processor SIGABRTs, or
# (c) the next reader sees a corrupt list.
#
# Variants this matters most for: tsan (deterministic race repro),
# asan (heap UAF on the same path).
#
# Tunables:
#   IT_BURST_N   — number of RUNs to send (default 100)
#   IT_PIPELINE  — pipeline name (default demo-chain)

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "serve_all_cache_hit_burst"
require "$SCEN_BIN" node jq

N="${IT_BURST_N:-100}"
PIPELINE="${IT_PIPELINE:-demo-chain}"
INPUT='{"base":5}'

PORT=$(alloc_port)
start_processor_serve "$PORT" --pipelines-dir "$SCEN_PIPELINES_DIR/smoke"

URL="ws://127.0.0.1:$PORT"

OUT=$("$SCEN_CLIENTS_DIR/node-ws.js" \
        --url "$URL" \
        --command burst \
        --pipeline "$PIPELINE" \
        --input "$INPUT" \
        --subscribe report \
        --n "$N" \
        --timeout-ms 60000)

EXEC_TOTAL=$(jq -r '.steps_executed_total' <<<"$OUT")
HIT_TOTAL=$(jq -r '.cache_hits_total'     <<<"$OUT")

# First run executes the pipeline; subsequent N-1 runs all hit.
# demo-chain has 4 steps.
EXPECTED_EXEC=4
EXPECTED_HITS_MIN=$(( (N - 1) * 4 ))
# Upper bound is loose: nested cached_compute hits inside individual steps
# (the cube-memoized fan-out, B3.5) bring extra hits that aren't a defect;
# under ASan the schedule shifts the count slightly upward. The bug class
# this scenario guards (BUG-cache-hit-runner-cleanup.md) manifests as
# crash or missing/duplicated entries, not as inflated hit counts.
EXPECTED_HITS_MAX=$(( N * 4 * 4 ))

[[ "$EXEC_TOTAL" == "$EXPECTED_EXEC" ]] \
    || fail "expected $EXPECTED_EXEC steps executed total, got $EXEC_TOTAL"
[[ "$HIT_TOTAL" -ge "$EXPECTED_HITS_MIN" && "$HIT_TOTAL" -le "$EXPECTED_HITS_MAX" ]] \
    || fail "expected cache hits in [$EXPECTED_HITS_MIN, $EXPECTED_HITS_MAX], got $HIT_TOTAL"

# Processor must still be alive.
if ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    fail "processor died during burst — likely sanitizer-caught race"
fi

assert_no_sanitizer_reports

pass "burst N=$N exec=$EXEC_TOTAL hits=$HIT_TOTAL"
