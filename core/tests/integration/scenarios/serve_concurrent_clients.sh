#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# serve_concurrent_clients
#
# Bug class: callback-fan-out races on a shared session_mgr (sessions
# table reads/writes from many concurrent clients) AND on per-session
# state (multiple sessions running in parallel each fan callbacks
# through their own session lock).  Generally exercises the
# sessions_lock → session_t::lock nesting documented in
# CONCURRENCY.md §12.3.
#
# Approach: open N parallel WS sockets, fire identical RUN on each at
# roughly the same instant via Promise.all in the node client.
# Identical input means most runs are cache-hit AFTER the first one
# wins the cold path.
#
# Variants this matters most for: tsan (lock-order, race on shared
# manager state), asan (heap correctness on parallel callback fan-out).
#
# Tunables:
#   IT_PARALLEL_N — number of concurrent clients (default 8)

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "serve_concurrent_clients"
require "$SCEN_BIN" node jq

N="${IT_PARALLEL_N:-8}"
PIPELINE="demo-chain"
INPUT='{"base":7}'

PORT=$(alloc_port)
start_processor_serve "$PORT" --pipelines-dir "$SCEN_PIPELINES_DIR/smoke"

URL="ws://127.0.0.1:$PORT"

OUT=$("$SCEN_CLIENTS_DIR/node-ws.js" \
        --url "$URL" \
        --command parallel-runs \
        --pipeline "$PIPELINE" \
        --input "$INPUT" \
        --subscribe report \
        --n "$N" \
        --timeout-ms 60000)

# Every client should have completed.  Some will be all-cache-hit
# (whichever finishes after the first one populates the cache); the
# first one (and possibly any that started before the cache was warm)
# will execute steps.  We don't assert on the exact split — the
# concurrent miss → put → hit interleaving is intentionally racy.
# What we do assert: every client got a RUN_COMPLETE, and the
# processor is still alive.

OK_COUNT=$(jq -r '.results | length' <<<"$OUT")
[[ "$OK_COUNT" == "$N" ]] || fail "expected $N completed runs, got $OK_COUNT"

if ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    fail "processor died during concurrent runs"
fi

assert_no_sanitizer_reports

pass "N=$N parallel runs all completed; processor alive"
