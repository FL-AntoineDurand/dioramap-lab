#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# serve_cancel_during_run
#
# Bug class: CANCEL races the runner's worker callbacks.  Under
# CONCURRENCY.md §12.5, runner_cancel takes runner::active_lock then
# rs->lock (strict LIFO) while workers may be mid-callback into the
# session.  Anything wrong with that ordering or with the post-cancel
# session_destroy path surfaces here.
#
# Approach: open WS, send RUN, wait a short time so the run is
# in-flight, send CANCEL.  Outcome: either RUN_COMPLETE (race won by
# the run) or ERROR(cancelled).  Both acceptable; what matters is the
# processor stays up and no sanitizer fires.
#
# Variants this matters most for: tsan (cancel/callback race), asan
# (UAF on session state during the cancel-then-cleanup window).

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "serve_cancel_during_run"
require "$SCEN_BIN" node jq

PIPELINE="demo-chain"

PORT=$(alloc_port)
start_processor_serve "$PORT" --pipelines-dir "$SCEN_PIPELINES_DIR/smoke"

URL="ws://127.0.0.1:$PORT"

# Run the cancel flow several times so we hit different timing
# offsets — the race window is small.
for i in $(seq 1 5); do
    OUT=$("$SCEN_CLIENTS_DIR/node-ws.js" \
            --url "$URL" \
            --command cancel-after \
            --pipeline "$PIPELINE" \
            --input "{\"base\":$i}" \
            --subscribe report \
            --cancel-after-ms 20 \
            --timeout-ms 15000)
    OK=$(jq -r '.ok' <<<"$OUT")
    [[ "$OK" == "true" ]] || fail "cancel iteration $i failed: $OUT"
    OUTCOME=$(jq -r '.outcome' <<<"$OUT")
    case "$OUTCOME" in
        complete|error|closed) ;;   # all acceptable; what matters is no crash
        *) fail "iteration $i: unexpected outcome '$OUTCOME'" ;;
    esac
done

if ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    fail "processor died during cancel iterations"
fi

assert_no_sanitizer_reports

pass "5 cancel iterations completed; processor alive"
