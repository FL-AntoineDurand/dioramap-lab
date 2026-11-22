#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Run the Frama-C audit across every module in modules.tsv and every
# bench in v7-benches.tsv, emitting a JUnit XML file.
#
# One JUnit testcase per audit target.
#   pass = audit-*.sh exit 0  AND no "sure alarm (invalid status)" in eva.log
#   fail = exit != 0 OR sure alarm present (eva.log tail attached)
#
# We also extract the "Unproven properties (count): N" digest line as
# a property on each testcase so the dashboard can plot the trend.
#
# Usage: run-frama-c.sh <out.xml> [<log-dir>]
#   <log-dir> defaults to .frama-c-out/ which is where the audit scripts
#   already write their outputs.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"
. "$REPO_ROOT/scripts/ci/lib/junit.sh"

OUT="${1:?usage: run-frama-c.sh <out.xml> [log-dir]}"
LOG_ROOT="${2:-$REPO_ROOT/.frama-c-out}"
mkdir -p "$(dirname "$OUT")"
mkdir -p "$LOG_ROOT"

junit_begin "frama-c"

overall_rc=0

out_dir_for() {
    if [[ "$1" == module ]]; then
        echo "$LOG_ROOT/$2"
    else
        echo "$LOG_ROOT/v7-bench-$2"
    fi
}

# Worker: runs one audit target end-to-end and writes its results to
# per-task files (.tier-line / .junit-* / .rc) inside its out_dir so the
# orchestrator can replay them in deterministic order after `wait` — bash
# in-memory arrays (e.g. __JUNIT_RECORDS in junit.sh) don't cross fork
# boundaries, so we serialise via filesystem instead. Self-contained: the
# orchestrator's `overall_rc` is derived from per-task .rc files.
run_one() {
    local target="$1" key="$2"

    local script_path arg out_dir
    if [[ "$target" == module ]]; then
        script_path="scripts/frama-c/audit-module.sh"
    else
        script_path="scripts/frama-c/audit-v7-bench.sh"
    fi
    out_dir="$(out_dir_for "$target" "$key")"
    arg="$key"

    local eva_log="$out_dir/eva.log"
    local summary="$out_dir/summary.txt"
    local wrapper_log="$LOG_ROOT/wrapper-${target}-${key}.log"

    local start_ns end_ns duration rc
    start_ns=$(date +%s%N)
    set +e
    bash "$script_path" "$arg" >"$wrapper_log" 2>&1
    rc=$?
    set -e
    end_ns=$(date +%s%N)
    duration=$(awk "BEGIN{print ($end_ns - $start_ns) / 1000000000}")

    # Pass/fail rule reads the audit script's own outputs (eva.log + summary.txt
    # under <out_dir>/), not the wrapper log; the wrapper log is just kept on
    # disk in case the audit script died before writing its own files.
    local sure_alarm=""
    if [[ -f "$eva_log" ]]; then
        sure_alarm=$(grep -m1 "sure alarm (invalid status)" "$eva_log" || true)
    fi
    local unproven="?"
    if [[ -f "$summary" ]]; then
        unproven=$(grep -oE 'Unproven properties \(count\): [0-9]+' "$summary" | awk '{print $NF}')
    fi

    # Attach the eva.log if present, else the wrapper log.
    local log_attach="${eva_log}"
    [[ -f "$log_attach" ]] || log_attach="$wrapper_log"

    local case_name="${target}-${key}"

    # Effective pass = analysis demonstrably completed + no sure alarm.
    #   - rc=0 is the happy path.
    #   - rc=141 (SIGPIPE) can fire on the largest modules (e.g.
    #     v7_br_impl with 6269 properties) AFTER eva has finished and the
    #     alarm CSV is on disk — frama-c gets killed on an internal pipe
    #     close during its final exit sequence. Accept it iff the
    #     "[report] Dumping properties" marker (written at the very end
    #     of frama-c's pipeline, after the alarm-CSV write) is present.
    #     We do NOT accept rc=141 without that marker — that would mask a
    #     real crash mid-analysis.
    local eva_completed=""
    if [[ -f "$eva_log" ]]; then
        eva_completed=$(grep -m1 '\[report\] Dumping properties' "$eva_log" || true)
    fi
    local effective_pass=0
    if [[ -z "$sure_alarm" && -n "$eva_completed" ]]; then
        if [[ "$rc" -eq 0 || "$rc" -eq 141 ]]; then
            effective_pass=1
        fi
    fi

    mkdir -p "$out_dir"
    if [[ "$effective_pass" -eq 1 ]]; then
        echo "[frama-c] $case_name :: PASS  (unproven=$unproven, ${duration}s)" > "$out_dir/.tier-line"
        printf '%s\n' "$case_name" > "$out_dir/.junit-name"
        printf '%s\n' "pass"       > "$out_dir/.junit-status"
        printf '%s\n' "$duration"  > "$out_dir/.junit-duration"
        printf '%s\n' "$log_attach" > "$out_dir/.junit-log"
        : > "$out_dir/.junit-fmsg"
        echo 0 > "$out_dir/.rc"
    else
        local fmsg
        if [[ -n "$sure_alarm" ]]; then
            fmsg="$sure_alarm"
        else
            fmsg=$(tail -3 "$log_attach" 2>/dev/null | tr '\n' ' ' | head -c 200)
        fi
        echo "[frama-c] $case_name :: FAIL  (rc=$rc, ${duration}s) — $fmsg" > "$out_dir/.tier-line"
        printf '%s\n' "$case_name" > "$out_dir/.junit-name"
        printf '%s\n' "fail"       > "$out_dir/.junit-status"
        printf '%s\n' "$duration"  > "$out_dir/.junit-duration"
        printf '%s\n' "$log_attach" > "$out_dir/.junit-log"
        printf '%s\n' "$fmsg"      > "$out_dir/.junit-fmsg"
        echo 1 > "$out_dir/.rc"
    fi
}

# ── Collect tasks in deterministic order (modules.tsv then v7-benches.tsv) ──
TASKS=()
while IFS=$'\t' read -r key _label _sources _harness; do
    [[ -z "$key" || "$key" == "key" ]] && continue
    TASKS+=("module|$key")
done < scripts/frama-c/modules.tsv
while IFS=$'\t' read -r bench _deps; do
    [[ -z "$bench" || "$bench" == "bench" ]] && continue
    TASKS+=("bench|$bench")
done < scripts/frama-c/v7-benches.tsv

# ── Parallel dispatch ──────────────────────────────────────────────────
# Default to min(nproc, 4) workers. Each Frama-C/Eva run peaks around
# 1–2 GB on the bigger modules (v7_br_impl with 6269 properties is the
# outlier); 4 workers × ~2 GB fits the 7 GB QA VM with headroom. Override
# with DIORAMAP_FRAMAC_JOBS=N (e.g. =1 to debug serialise, =6 on a fat
# CI runner). The big win here is amortising the small-module audits
# (each ~1–6 s × 30+ modules) against the long pole (v7_br_impl ~700 s)
# — without parallelism those small audits add ~190 s of pure serial wait
# *after* v7_br_impl finishes.
JOBS_DEFAULT=$(nproc 2>/dev/null || echo 4)
[[ "$JOBS_DEFAULT" -gt 4 ]] && JOBS_DEFAULT=4
JOBS=${DIORAMAP_FRAMAC_JOBS:-$JOBS_DEFAULT}
echo "[frama-c] parallel: $JOBS worker(s); ${#TASKS[@]} task(s)"

pids=()
for t in "${TASKS[@]}"; do
    while [[ ${#pids[@]} -ge $JOBS ]]; do
        local_new=()
        for p in "${pids[@]}"; do
            kill -0 "$p" 2>/dev/null && local_new+=("$p")
        done
        pids=("${local_new[@]}")
        [[ ${#pids[@]} -ge $JOBS ]] && sleep 0.3
    done
    IFS='|' read -r tgt key <<< "$t"
    run_one "$tgt" "$key" &
    pids+=("$!")
done
wait

# ── Replay per-task results in deterministic order ─────────────────────
# Reading each per-task file separately avoids any delimiter issues with
# fmsg (which can contain pipes, quotes, etc.).
for t in "${TASKS[@]}"; do
    IFS='|' read -r tgt key <<< "$t"
    out_dir="$(out_dir_for "$tgt" "$key")"
    [[ -f "$out_dir/.tier-line" ]] && cat "$out_dir/.tier-line"
    if [[ -f "$out_dir/.junit-name" ]]; then
        junit_record \
            "$(< "$out_dir/.junit-name")" \
            "$(< "$out_dir/.junit-status")" \
            "$(< "$out_dir/.junit-duration")" \
            "$(< "$out_dir/.junit-log")" \
            "$(< "$out_dir/.junit-fmsg")"
    fi
    if [[ -f "$out_dir/.rc" && "$(< "$out_dir/.rc")" != "0" ]]; then
        overall_rc=1
    fi
done

junit_end "$OUT"
echo "[frama-c] → $OUT"
exit "$overall_rc"
