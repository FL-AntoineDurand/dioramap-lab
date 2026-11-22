#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# serve_list_runs
#
# Coverage target: WS LIST_RUNS handler + run_journal_list +
# run_journal recording path.
#
# Approach: RUN with a unique test_case_label, then LIST_RUNS.
# Assert the row with that label appears, and that its
# pipeline / input_hash / success / steps_executed / cache_hits
# fields are populated.  Schema-level validation of the journal
# round-trip.

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "serve_list_runs"
require "$SCEN_BIN" node jq

PORT=$(alloc_port)
start_processor_serve "$PORT" --pipelines-dir "$SCEN_PIPELINES_DIR/smoke"

URL="ws://127.0.0.1:$PORT"

OUT=$("$SCEN_CLIENTS_DIR/node-ws.js" \
        --url "$URL" \
        --command list-runs \
        --pipeline demo-chain \
        --input '{"base":42}' \
        --subscribe report \
        --timeout-ms 30000)

OK=$(jq -r '.ok' <<<"$OUT")
[[ "$OK" == "true" ]] || fail "list-runs flow failed: $OUT"

# At least one row, the most recent should be ours.
N_ROWS=$(jq -r '.runs | length' <<<"$OUT")
(( N_ROWS >= 1 )) || fail "expected ≥1 journal row, got $N_ROWS"

ROW=$(jq -r '.runs[] | select(.test_case_label == "integration-list-runs")' <<<"$OUT")
[[ -n "$ROW" ]] || fail "no row with our test_case_label; got: $OUT"

PIPE=$(jq -r '.pipeline'      <<<"$ROW")
SUCCESS=$(jq -r '.success'    <<<"$ROW")
EXEC=$(jq -r '.n_executed'    <<<"$ROW")
HITS=$(jq -r '.n_cache_hits'  <<<"$ROW")

[[ "$PIPE" == "demo-chain" ]] || fail "row pipeline != demo-chain: $PIPE"
[[ "$SUCCESS" == "true"    ]] || fail "row success != true: $SUCCESS"
# First run is cold: 4 executed, 0 cached.
[[ "$EXEC" == "4" && "$HITS" == "0" ]] \
    || fail "row exec/hits expected 4/0, got $EXEC/$HITS"

assert_no_sanitizer_reports

pass "LIST_RUNS returned $N_ROWS row(s); ours has expected schema"
