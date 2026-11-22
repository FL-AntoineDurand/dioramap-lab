#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# serve_disconnect_during_run
#
# Bug class: client closes the WS abruptly mid-run.  The grace path
# (session detached, run cancelled, session retained for reconnect)
# must execute cleanly with worker callbacks still in-flight.  This
# is the LWS_CALLBACK_CLOSED → session_mgr_disconnect path through
# the encapsulated session_t API.
#
# Approach: send RUN, wait briefly, terminate the socket without
# closing it gracefully (no FIN).  Server must:
#   - cancel the run,
#   - retain the session for grace_sec,
#   - not crash, leak, or sanitizer-abort.
#
# Variants this matters most for: asan (UAF during the cancel/detach
# window), tsan (cancel races with worker callbacks).

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "serve_disconnect_during_run"
require "$SCEN_BIN" node

PIPELINE="demo-chain"

PORT=$(alloc_port)
start_processor_serve "$PORT" --pipelines-dir "$SCEN_PIPELINES_DIR/smoke"

URL="ws://127.0.0.1:$PORT"

for i in $(seq 1 5); do
    "$SCEN_CLIENTS_DIR/node-ws.js" \
        --url "$URL" \
        --command close-after \
        --pipeline "$PIPELINE" \
        --input "{\"base\":$i}" \
        --subscribe report \
        --close-after-ms 30 \
        --timeout-ms 10000 \
        >/dev/null
done

# Give the runner a moment to drain post-cancel callbacks.
sleep 1

if ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    fail "processor died after abrupt disconnects"
fi

assert_no_sanitizer_reports

pass "5 abrupt disconnects survived; processor alive"
