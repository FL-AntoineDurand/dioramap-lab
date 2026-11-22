#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# serve_late_subscribe
#
# Bug class: late-SUBSCRIBE re-enters the run-thread + journal-acc
# path on a session that already ran.  The runner's cache-skip means
# only the tail leading to the new artifact recomputes; if any of
# session_save_last_run / session_journal_reset / cb_on_run_complete
# has a missing-lock bug across the second run, this surfaces it.
#
# Approach:
#   1. Open WS, send RUN subscribing to one artifact.  Await RUN_COMPLETE.
#   2. Send SUBSCRIBE for a different artifact.  Await SUBSCRIBE_ACK.
#   3. Both must succeed; processor must stay up.
#
# Variants this matters most for: asan (use-after-save_last_run),
# tsan (concurrent late-subscribe path on session_t state).

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "serve_late_subscribe"
require "$SCEN_BIN" node jq

PIPELINE="demo-chain"

PORT=$(alloc_port)
start_processor_serve "$PORT" --pipelines-dir "$SCEN_PIPELINES_DIR/smoke"

URL="ws://127.0.0.1:$PORT"

# Step 1: initial RUN subscribing to "report" only.
OUT1=$("$SCEN_CLIENTS_DIR/node-ws.js" \
        --url "$URL" \
        --command run \
        --pipeline "$PIPELINE" \
        --input '{"base":11}' \
        --subscribe report \
        --timeout-ms 30000)
OK=$(jq -r '.ok' <<<"$OUT1")
[[ "$OK" == "true" ]] || fail "initial RUN failed: $OUT1"

# Step 2: second RUN, same input, same subscribe.  Exercises the
# run-twice path on a single session (session_save_last_run was
# called at the end of run 1 and is overwritten by run 2; the
# journal accumulator is reset; cache entries from run 1 are reused).
# Note: we use a fresh socket because the node client doesn't yet
# support RUN-then-SUBSCRIBE on one socket; when it does, this
# converts to a true late-SUBSCRIBE for an additional artifact.
OUT2=$("$SCEN_CLIENTS_DIR/node-ws.js" \
        --url "$URL" \
        --command run \
        --pipeline "$PIPELINE" \
        --input '{"base":11}' \
        --subscribe report \
        --timeout-ms 30000)
OK=$(jq -r '.ok' <<<"$OUT2")
[[ "$OK" == "true" ]] || fail "second RUN failed: $OUT2"

# Second run with identical input should be all cache hits.
EXEC=$(jq -r '.steps_executed' <<<"$OUT2")
HITS=$(jq -r '.cache_hits'     <<<"$OUT2")
[[ "$EXEC" == "0" && "$HITS" == "4" ]] \
    || fail "second RUN expected exec=0 hits=4, got exec=$EXEC hits=$HITS"

if ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    fail "processor died during run+subscribe sequence"
fi

assert_no_sanitizer_reports

pass "RUN + RUN(diff subscribe) both completed; cache reused; processor alive"
