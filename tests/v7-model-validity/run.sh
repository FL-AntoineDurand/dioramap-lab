#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# tests/v7-model-validity/run.sh
#
# Run the v7 model validity tier against a chosen subset of bench-v2
# synthetic test cases.  For each case:
#   1. materialize the synthetic input (point cloud + analytic ground truth)
#   2. run v3's v7 pipeline against it
#   3. compute the metrics defined in tests/v7-model-validity/metrics/
#   4. accumulate into a per-run scorecard JSON
#
# Usage:
#   ./tests/v7-model-validity/run.sh <tier>     (tier ∈ easy|standard|hard|all)
#   ./tests/v7-model-validity/run.sh <tier> <artifact-dir>
#
# Exit 0 iff every case passes every gated metric.  Today (B.1 skeleton),
# the only gated metric is plane_count_match on the easy tier.

set -uo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SELF_DIR/../.." && pwd)"

TIER="${1:?usage: run.sh <tier> [artifact-dir]}"
if [[ -n "${2:-}" ]]; then
    ART="$2"
else
    ts=$(date +%Y%m%d-%H%M%S)
    ART="/tmp/v7mv-run-$ts"
fi
mkdir -p "$ART"

CASES_FILE="$SELF_DIR/cases/${TIER}.txt"
[[ -f "$CASES_FILE" ]] || { echo "no case list at $CASES_FILE" >&2; exit 2; }

# Pick the v3 binary. The research-v7 pipeline + v7-load-real-test-case step
# live in dioramap_steps_v7, built whenever DIORAMAP_V7=ON (the default), so
# any standard build dir carries them — DIORAMAP_RESEARCH is NOT required.
BIN="$REPO_ROOT/core/build-default/dioramap-processor"
if [[ ! -x "$BIN" ]]; then
    BIN="$REPO_ROOT/core/build-release/dioramap-processor"
fi
[[ -x "$BIN" ]] || { echo "no dioramap-processor binary found" >&2; exit 2; }

PIPELINE_TEMPLATE="$REPO_ROOT/pipelines/smoke/v7.json"
[[ -f "$PIPELINE_TEMPLATE" ]] || { echo "missing $PIPELINE_TEMPLATE" >&2; exit 2; }

echo "[v7mv] tier=$TIER  bin=${BIN#$REPO_ROOT/}  out=$ART"
echo

# Per-case scorecards accumulated here, merged at the end.
SCORECARDS=()
OVERALL_RC=0

while IFS= read -r line; do
    case_id="${line%%#*}"
    case_id="${case_id## }"
    case_id="${case_id%% }"
    [[ -z "$case_id" ]] && continue

    case_dir="$ART/$case_id"
    mkdir -p "$case_dir"
    echo "[v7mv] case=$case_id"

    # ── 1. materialize ────────────────────────────────────────────────
    mat_out=$(node "$SELF_DIR/tools/materialize-case.mjs" "$case_id" "$case_dir" 2>"$case_dir/materialize.err")
    if [[ $? -ne 0 ]]; then
        echo "[v7mv]   FAIL materialize: $(cat "$case_dir/materialize.err" | head -3)"
        SCORECARDS+=("$(jq -n --arg c "$case_id" '{case_id:$c, passed:false, error:"materialize-failed"}')")
        OVERALL_RC=1
        continue
    fi
    spec_path=$(jq -r '.spec' <<<"$mat_out")
    gt_path=$(jq -r '.ground_truth' <<<"$mat_out")

    # ── 2. generate per-case pipeline JSON, swapping in our spec ──────
    pipeline_out="$case_dir/pipeline.json"
    python3 -c "
import json, sys
p = json.load(open('$PIPELINE_TEMPLATE'))
# First step is v7-load-real-test-case; override its spec param.
for s in p['steps']:
    if s.get('step') == 'v7-load-real-test-case':
        s.setdefault('params', {})['spec'] = '$spec_path'
        break
json.dump(p, open('$pipeline_out', 'w'), indent=2)
"

    # ── 3. run v3 v7 ──────────────────────────────────────────────────
    out_dir="$case_dir/v3-out"
    cache_dir="$case_dir/v3-cache"
    mkdir -p "$out_dir" "$cache_dir"
    run_log="$case_dir/v3-run.log"

    # boundary-merge was retired in #76; the closest equivalent for
    # plane_count_match is boundary-classify-roof-graph (the latest
    # RGRAPH TLV in the pipeline — passes PLANES through unchanged from
    # upstream).
    "$BIN" batch \
        --pipeline "$pipeline_out" \
        --input-json '{}' \
        --produce boundary-classify-roof-graph \
        --output-dir "$out_dir" \
        --cache-dir "$cache_dir" \
        >"$run_log" 2>&1
    rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "[v7mv]   FAIL v3 pipeline rc=$rc (see $run_log)"
        SCORECARDS+=("$(jq -n --arg c "$case_id" --arg rc "$rc" '{case_id:$c, passed:false, error:"pipeline-rc-\($rc)"}')")
        OVERALL_RC=1
        continue
    fi

    roof_graph="$out_dir/boundary-classify-roof-graph.bin"
    if [[ ! -f "$roof_graph" ]]; then
        echo "[v7mv]   FAIL pipeline produced no roof-graph"
        SCORECARDS+=("$(jq -n --arg c "$case_id" '{case_id:$c, passed:false, error:"no-roof-graph"}')")
        OVERALL_RC=1
        continue
    fi

    # ── 4. score ──────────────────────────────────────────────────────
    # Native C tool (v3(#73)) — reuses the pipeline's own roof-graph
    # reader + `rg_plane_is_wall`, so metric semantics stay in lockstep.
    # See v3/tests/v7-model-validity/tools/score-vs-gt.c. Built alongside
    # the v3 processor; located in the same build dir.
    SCORER="$(dirname "$BIN")/v7mv-score-vs-gt"
    if [[ ! -x "$SCORER" ]]; then
        echo "[v7mv]   FAIL scorer not built at $SCORER"
        SCORECARDS+=("$(jq -n --arg c "$case_id" '{case_id:$c, passed:false, error:"scorer-missing"}')")
        OVERALL_RC=1
        continue
    fi
    cmp_out=$("$SCORER" "$roof_graph" "$gt_path" 2>"$case_dir/compare.err")
    cmp_rc=$?
    if [[ -z "$cmp_out" ]]; then
        echo "[v7mv]   FAIL scorer emitted no output"
        SCORECARDS+=("$(jq -n --arg c "$case_id" '{case_id:$c, passed:false, error:"compare-empty"}')")
        OVERALL_RC=1
        continue
    fi
    SCORECARDS+=("$cmp_out")
    if [[ $cmp_rc -eq 0 ]]; then
        echo "[v7mv]   PASS  $cmp_out"
    else
        echo "[v7mv]   FAIL  $cmp_out"
        OVERALL_RC=1
    fi
done < "$CASES_FILE"

# ── 5. emit scorecard ─────────────────────────────────────────────────
python3 "$SELF_DIR/scorecard.py" "$TIER" "$ART/scorecard.json" "${SCORECARDS[@]}"
echo
echo "[v7mv] scorecard → $ART/scorecard.json"
if [[ $OVERALL_RC -eq 0 ]]; then
    echo "[v7mv] $TIER PASS"
else
    echo "[v7mv] $TIER FAIL"
fi
exit "$OVERALL_RC"
