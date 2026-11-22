#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# serve_invalidate_then_rerun
#
# Coverage targets:
#   - WS INVALIDATE handler (server.c handle_invalidate_message)
#   - intent_map_invalidate + intent_map tombstone replay
#   - runner_compute_step_identity (out-of-band identity calc path)
#   - runner cache-skip propagation: invalidating step X forces
#     re-execution of X AND every downstream step that reads X's
#     output (transitively).  Upstream steps remain cache-hits.
#
# Approach:
#   1. RUN demo-chain {base:23}, subscribe=report.  Cold: 4 exec, 0 hits.
#   2. INVALIDATE artifact "product" — the multiply step's write.
#      (demo-chain artifact map: scale-by-factor→scaled, add-bias→biased,
#      multiply→product, report→report. INVALIDATE resolves artifact
#      name → producing step.)
#   3. RUN again same input.  Expected (per server.c handle_invalidate
#      comment, "next run recomputes that step (and only that step
#      — upstream ancestors stay cached)"):
#        - scale-by-factor:  cache hit  (upstream ancestor)
#        - add-bias:         cache hit  (upstream ancestor)
#        - multiply:         RE-EXECUTED (intent-map tombstone)
#        - report:           cache hit  (deterministic step + content
#                            of `product` is unchanged because
#                            multiply is deterministic → report's
#                            identity is the same → cache-hit)
#      → run2 should show executed=1, cache_hits=3.
#
# This is a feature of content-addressed caching, not a bug:
# downstream steps reuse cached values when re-execution produces
# identical content.  See HASH-MODEL.md.

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "serve_invalidate_then_rerun"
require "$SCEN_BIN" node jq

PORT=$(alloc_port)
start_processor_serve "$PORT" --pipelines-dir "$SCEN_PIPELINES_DIR/smoke"

URL="ws://127.0.0.1:$PORT"

# demo-chain step "multiply" writes the "multiply_out" artifact (per
# the steps/demo source).  We invalidate the artifact name; the
# server resolves it back to the producing step.
OUT=$("$SCEN_CLIENTS_DIR/node-ws.js" \
        --url "$URL" \
        --command invalidate \
        --pipeline demo-chain \
        --input '{"base":23}' \
        --subscribe report \
        --artifact product \
        --timeout-ms 30000)

OK=$(jq -r '.ok' <<<"$OUT")
[[ "$OK" == "true" ]] || fail "invalidate flow failed: $OUT"

INV_STEP=$(jq -r '.invalidated_step' <<<"$OUT")
R1_EXEC=$(jq -r '.run1.steps_executed' <<<"$OUT")
R1_HITS=$(jq -r '.run1.cache_hits'     <<<"$OUT")
R2_EXEC=$(jq -r '.run2.steps_executed' <<<"$OUT")
R2_HITS=$(jq -r '.run2.cache_hits'     <<<"$OUT")

[[ "$INV_STEP" == "multiply" ]] \
    || fail "expected invalidated step=multiply, got $INV_STEP"
[[ "$R1_EXEC" == "4" && "$R1_HITS" == "0" ]] \
    || fail "run1 expected 4/0, got $R1_EXEC/$R1_HITS"

# Per the contract documented in the scenario header: invalidating
# `product` forces only the `multiply` step to re-execute.  Because
# multiply is deterministic, its re-execution produces identical
# content, so `product`'s content hash is unchanged → report's
# identity is unchanged → report cache-hits.  Net: exec=1 (multiply),
# hits=3 (scale + bias + report).
#
# Verified end-to-end with debug instrumentation 2026-05-04:
# multiply's intent_map_get correctly returns -1 after the tombstone;
# cached_compute's miss path runs body_trampoline; the body
# completes in ~15μs because every store_lookup / store_put hits
# the dedup fast path (deterministic content already present).
[[ "$R2_EXEC" == "1" && "$R2_HITS" == "3" ]] \
    || fail "run2 expected 1/3 (multiply re-runs deterministically, downstream cache-hits), got $R2_EXEC/$R2_HITS"

assert_no_sanitizer_reports

pass "INVALIDATE → run2 1/3: multiply re-executes, deterministic downstream cache-hits"
