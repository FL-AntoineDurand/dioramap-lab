#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# serve_reconnect_within_grace
#
# Coverage targets:
#   - WS RECONNECT handler (server.c handle_reconnect_message)
#   - session_mgr_reattach (the lock-nesting site:
#     sessions_lock → session_t::lock — see CONCURRENCY.md §12.3)
#   - session_set_wsi (clears disconnected flag, drains stale queue)
#   - Pin lifetime across the disconnect/reconnect window: store pins
#     taken on the original session must survive grace and be
#     reachable by the reconnected session's follow-up SUBSCRIBE.
#   - Grace path in session_mgr_disconnect (session retained, not
#     destroyed, when grace_sec > 0).
#
# Approach:
#   1. Connect, capture session_token from SESSION_STARTED.
#   2. RUN demo-chain {base:31}, subscribe=report.  Pins land.
#   3. terminate() the socket abruptly — exercise the grace path
#      (CLOSED → session_mgr_disconnect → mark_disconnected, NOT destroy).
#   4. Brief sleep so the server's CLOSED handler runs.
#   5. New socket, send RECONNECT { session_token }.
#      Expect RECONNECTED with the SAME token.
#   6. Send a follow-up RUN, same input.  Expect all-cache-hit
#      (the original run's pinned artifacts are still warm).

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "serve_reconnect_within_grace"
require "$SCEN_BIN" node jq

PORT=$(alloc_port)
start_processor_serve "$PORT" --pipelines-dir "$SCEN_PIPELINES_DIR/smoke"

URL="ws://127.0.0.1:$PORT"

OUT=$("$SCEN_CLIENTS_DIR/node-ws.js" \
        --url "$URL" \
        --command reconnect-flow \
        --pipeline demo-chain \
        --input '{"base":31}' \
        --subscribe report \
        --timeout-ms 30000)

OK=$(jq -r '.ok' <<<"$OUT")
[[ "$OK" == "true" ]] || fail "reconnect flow failed: $OUT"

TP=$(jq -r '.token_preserved'      <<<"$OUT")
R1_EXEC=$(jq -r '.run1.steps_executed' <<<"$OUT")
R1_HITS=$(jq -r '.run1.cache_hits'     <<<"$OUT")
R2_EXEC=$(jq -r '.run2.steps_executed' <<<"$OUT")
R2_HITS=$(jq -r '.run2.cache_hits'     <<<"$OUT")

[[ "$TP" == "true" ]] || fail "session_token not preserved across reconnect"
[[ "$R1_EXEC" == "4" && "$R1_HITS" == "0" ]] \
    || fail "run1 expected 4/0, got $R1_EXEC/$R1_HITS"
[[ "$R2_EXEC" == "0" && "$R2_HITS" == "4" ]] \
    || fail "run2 (post-reconnect) expected 0/4, got $R2_EXEC/$R2_HITS"

assert_no_sanitizer_reports

pass "RECONNECT preserved token; post-reconnect run is all-cache-hit"
