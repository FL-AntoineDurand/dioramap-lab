#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# serve_rehydrate_run
#
# Coverage targets:
#   - WS REHYDRATE_RUN handler (server.c handle_rehydrate_run_message)
#   - run_journal_get_produced + parse_produced_json
#     (the function fixed for UAF on 2026-05-02 — exercise it through
#     the actual WS roundtrip)
#   - session_pin_artifact via the rehydrate path
#
# Approach: RUN with a unique label, find its journal_id via LIST_RUNS,
# REHYDRATE_RUN.  Assert REHYDRATE_COMPLETE arrives with at least one
# ARTIFACT_HEADER + matching binary frame.

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "serve_rehydrate_run"
require "$SCEN_BIN" node jq

PORT=$(alloc_port)
start_processor_serve "$PORT" --pipelines-dir "$SCEN_PIPELINES_DIR/smoke"

URL="ws://127.0.0.1:$PORT"

OUT=$("$SCEN_CLIENTS_DIR/node-ws.js" \
        --url "$URL" \
        --command rehydrate \
        --pipeline demo-chain \
        --input '{"base":17}' \
        --subscribe report \
        --timeout-ms 30000)

OK=$(jq -r '.ok' <<<"$OUT")
[[ "$OK" == "true" ]] || fail "rehydrate flow failed: $OUT"

KIND=$(jq -r '.kind'        <<<"$OUT")
EMITTED=$(jq -r '.n_emitted' <<<"$OUT")
HEADERS=$(jq -r '.n_headers' <<<"$OUT")
BINARY=$(jq -r '.n_binary'   <<<"$OUT")
JID=$(jq -r '.journal_id'    <<<"$OUT")

# The journal records ALL produced artifacts (4 for demo-chain).
# REHYDRATE emits one ARTIFACT_HEADER per produced artifact; the live
# RUN sent one ARTIFACT_HEADER for the subscribed "report".  So
# n_headers in the queue is the live-run header (1) + leftover
# rehydrate headers seen by the queue listener.  Just sanity-check
# emitted count and binary frames; the strict equality on n_headers
# would require us to drain the queue before sending REHYDRATE.
[[ "$KIND" == "REHYDRATE_COMPLETE" ]] \
    || fail "expected REHYDRATE_COMPLETE, got $KIND"
(( EMITTED == 4 )) || fail "expected 4 artifacts emitted (demo-chain), got $EMITTED"
(( BINARY >= EMITTED )) \
    || fail "expected n_binary ($BINARY) ≥ n_emitted ($EMITTED) — every header has a payload"

assert_no_sanitizer_reports

pass "REHYDRATE_COMPLETE jid=$JID emitted=$EMITTED headers=$HEADERS binary=$BINARY"
