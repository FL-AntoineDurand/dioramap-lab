#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Run one tier of the validation matrix end-to-end.
#
# Usage:  scripts/ci/run-tier.sh <fast|full|nightly|weekly> [<artifact-dir>]
#
# Produces a deterministic artifact tree:
#   <artifact-dir>/
#     summary.json
#     junit/*.xml
#     integration-logs-*/*.log
#     build-logs/*.log
#
# If <artifact-dir> is omitted, defaults to
#   ci-artifacts/YYYYMMDD-HHMMSS-<shortsha>/
# so multiple runs accumulate without overwriting.
#
# The same script runs locally and in CI; the only difference is that
# CI will upload the artifact dir as a workflow artifact.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# Run the whole tier at low CPU priority. A full build saturates every core
# (PolyFit/CGAL/roofer), which otherwise starves interactive sessions / SSH on
# the runner — to the point where you can't even Ctrl-C or kill it cleanly.
# `nice` only de-prioritises UNDER CONTENTION, so on an idle CI runner it's a
# no-op; correctness/timing of tests is unaffected. Children (cmake, ninja,
# compilers, ctest) inherit it. Override with DIORAMAP_TIER_NICE=<n>.
renice -n "${DIORAMAP_TIER_NICE:-19}" $$ >/dev/null 2>&1 || true

# Fail FAST if this host isn't a supported target. The toolchain versions are
# declared in scripts/ci/expected-toolchain.tsv; a run on a wrong-version box
# (e.g. a stale 24.04 container) aborts here instead of dying deep in a build.
# Override (not recommended) with DIORAMAP_SKIP_PRECHECK=1.
bash "$REPO_ROOT/scripts/ci/steps/precheck-toolchain.sh" || exit 1

. "$REPO_ROOT/scripts/ci/lib/variants.sh"

TIER="${1:?usage: run-tier.sh <fast|full|nightly|weekly> [artifact-dir]}"

# Default artifact dir uses timestamp + short SHA (if available).
if [[ -n "${2:-}" ]]; then
    ART="$2"
else
    ts=$(date +%Y%m%d-%H%M%S)
    sha=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
    ART="$REPO_ROOT/ci-artifacts/${ts}-${sha}-${TIER}"
fi
mkdir -p "$ART/junit" "$ART/build-logs"
echo "[tier] $TIER  artifacts → ${ART#$REPO_ROOT/}"

# ── prerequisite: third-party deps fetched ────────────────────────────
if [[ ! -d third_party/copc-lib/libs/laz-perf ]]; then
    echo "[tier] fetching third-party deps ..."
    ./scripts/fetch-third-party.sh
fi

OVERALL_RC=0
# Per-step timing: each record_step call appends "<seconds> <rc> <name>"
# to STEP_TIMINGS for the end-of-tier summary. Bash's SECONDS (integer)
# is enough — every step here takes >>1s. TIER_T0 captures wall start.
declare -a STEP_TIMINGS=()
TIER_T0=$SECONDS

record_step() {
    local name="$1" rc="$2" t="${3:-0}"
    if [[ "$rc" -eq 0 ]]; then
        printf '[tier] step %-22s :: PASS  (%4ds)\n' "$name" "$t"
    else
        printf '[tier] step %-22s :: FAIL  (rc=%d, %4ds)\n' "$name" "$rc" "$t"
        OVERALL_RC=1
    fi
    STEP_TIMINGS+=("$t $rc $name")
}

run_build() {
    local v="$1"
    local log="$ART/build-logs/${v}.log"
    local rc=0 t0=$SECONDS
    set +e
    bash scripts/ci/steps/build-variant.sh "$v" "$log"
    rc=$?
    set -e
    # Fail LOUDLY: a build's output goes to the log file, so on failure echo
    # its tail to the terminal instead of aborting silently (the old behaviour
    # — `run_build` bare under `set -e` — printed nothing and just returned).
    if [[ "$rc" -ne 0 ]]; then
        echo "[tier] build '$v' FAILED — tail of ${log#$REPO_ROOT/}:" >&2
        tail -30 "$log" >&2
    fi
    record_step "build-$v" "$rc" "$((SECONDS - t0))"
}

run_ctest() {
    local v="$1"
    local rc=0 t0=$SECONDS
    set +e
    bash scripts/ci/steps/run-ctest.sh \
        "$(variant_build_dir "$v")" \
        "$ART/junit/ctest-${v}.xml" \
        "$v" \
        >>"$ART/build-logs/ctest-${v}.log" 2>&1
    rc=$?
    set -e
    record_step "ctest-$v" "$rc" "$((SECONDS - t0))"
}

run_integration() {
    local v="$1"; shift
    local out_xml="$ART/junit/integration-${v}.xml"
    local rc=0 t0=$SECONDS
    set +e
    bash scripts/ci/steps/run-integration.sh \
        "$(variant_build_dir "$v")/dioramap-processor" \
        "$out_xml" \
        "$v" \
        "$@" \
        >>"$ART/build-logs/integration-${v}.log" 2>&1
    rc=$?
    set -e
    record_step "integration-$v" "$rc" "$((SECONDS - t0))"
    # Move integration-logs dir under artifacts for the dashboard:
    if [[ -d "$ART/junit/integration-logs-$v" ]]; then
        mv "$ART/junit/integration-logs-$v" "$ART/"
    fi
}

run_coverage() {
    local profraw="$ART/coverage/raw"
    local out_xml="$ART/junit/coverage.xml"
    local rc=0 t0=$SECONDS
    mkdir -p "$profraw"
    set +e
    bash scripts/ci/steps/run-coverage-sweep.sh "$profraw" "$out_xml" \
        >>"$ART/build-logs/cov-sweep.log" 2>&1
    rc=$?
    set -e
    record_step "coverage-sweep" "$rc" "$((SECONDS - t0))"

    local t1=$SECONDS
    set +e
    bash scripts/ci/steps/run-coverage-report.sh "$profraw" "$ART/coverage" \
        >>"$ART/build-logs/cov-report.log" 2>&1
    local rep_rc=$?
    set -e
    record_step "coverage-report" "$rep_rc" "$((SECONDS - t1))"
    # Coverage report failure shouldn't fail the tier — measurement, not gate.
    OVERALL_RC=$((OVERALL_RC | rc))   # only sweep failures count for tier RC
}

run_frama_c() {
    local out_xml="$ART/junit/frama-c.xml"
    local rc=0 t0=$SECONDS
    set +e
    bash scripts/ci/steps/run-frama-c.sh "$out_xml" \
        >>"$ART/build-logs/frama-c.log" 2>&1
    rc=$?
    set -e
    record_step "frama-c" "$rc" "$((SECONDS - t0))"
}

# Fuzz — regression-only in `full` (replay the checked-in corpus, gating,
# takes a couple of seconds per target). The "explore" mode (1 h per
# target) belongs in `nightly` once that tier exists (task #85).
run_fuzz() {
    local mode="${1:-regression}"
    local max_time="${2:-3600}"
    local out_xml="$ART/junit/fuzz.xml"
    local rc=0 t0=$SECONDS
    set +e
    bash scripts/ci/steps/run-fuzz.sh "$(variant_build_dir fuzz)" "$out_xml" \
        "$mode" "$max_time" \
        >>"$ART/build-logs/fuzz.log" 2>&1
    rc=$?
    set -e
    record_step "fuzz-$mode" "$rc" "$((SECONDS - t0))"
}

# Duplication detection. cpd-c is GATING since #72c — run-cpd.sh exits
# non-zero when any cluster exceeds CPD_GATE_TOKENS (default 300). jscpd
# (JS surface) is still report-only. cpd-c degrades gracefully when
# PMD/JRE are absent: the script exits 0 with a single skipped testcase.
run_cpd() {
    local out_xml="$ART/junit/cpd-c.xml"
    local rc=0 t0=$SECONDS
    set +e
    bash scripts/ci/steps/run-cpd.sh "$out_xml" "$ART/cpd-report" \
        >>"$ART/build-logs/cpd.log" 2>&1
    rc=$?
    set -e
    local t=$((SECONDS - t0))
    if [[ "$rc" -eq 0 ]]; then
        printf '[tier] step %-22s :: PASS  (%4ds)\n' "cpd-c" "$t"
    else
        printf '[tier] step %-22s :: FAIL  (rc=%d, %4ds — duplication over gate)\n' "cpd-c" "$rc" "$t"
        OVERALL_RC=1
    fi
    STEP_TIMINGS+=("$t $rc cpd-c")
}

run_jscpd() {
    local out_xml="$ART/junit/jscpd-js.xml"
    local rc=0 t0=$SECONDS
    set +e
    bash scripts/ci/steps/run-jscpd.sh "$out_xml" "$ART/jscpd-report" \
        >>"$ART/build-logs/jscpd.log" 2>&1
    rc=$?
    set -e
    local t=$((SECONDS - t0))
    if [[ "$rc" -eq 0 ]]; then
        printf '[tier] step %-22s :: PASS  (%4ds, report-only)\n' "jscpd-js" "$t"
    else
        printf '[tier] step %-22s :: WARN  (rc=%d, %4ds, report-only — not blocking)\n' "jscpd-js" "$rc" "$t"
    fi
    STEP_TIMINGS+=("$t 0 jscpd-js")  # never gates
}

# ── End-of-tier timing summary (longest-first). ───────────────────────
# Prints a sorted table of step durations to give the user an at-a-glance
# answer to "where did the wall time go?" — informs parallelization
# decisions (e.g. is frama-c the long pole? is a single build variant?).
print_step_timings() {
    local total_dt=$((SECONDS - TIER_T0))
    echo
    echo "[tier] step durations (longest first):"
    printf '%s\n' "${STEP_TIMINGS[@]}" \
        | sort -k1 -rn \
        | awk 'BEGIN{tot=0} {
            t=$1; rc=$2; $1=""; $2=""; name=$0; sub(/^  /,"",name);
            tot+=t;
            status = (rc=="0") ? "PASS" : "FAIL";
            printf "  %5ds  %s  %s\n", t, status, name;
        } END { printf "  %5ds  ----  steps subtotal\n", tot; }'
    printf "  %5ds  ----  total tier wall (incl. setup/summary)\n" "$total_dt"
}

case "$TIER" in
    fast)
        # Per docs/quality/README.md: < 5 min; release + asan;
        # essential scenarios (1 batch + journal-acc burst + concurrent
        # clients + late subscribe).
        run_build release
        run_ctest release
        run_build asan
        run_ctest asan
        run_integration asan \
            batch_demo_chain_warm \
            serve_all_cache_hit_burst \
            serve_concurrent_clients \
            serve_late_subscribe
        ;;
    full)
        # Per docs/quality/README.md: < 30 min; all variants,
        # all scenarios + coverage measurement + Frama-C static audit.
        #
        # TSan: was intentionally omitted because WSL2 refuses to launch
        # it (kernel won't disable ASLR via personality()
        # ADDR_NO_RANDOMIZE — see docs/quality/README.md "TSan on WSL2").
        # On real Linux it runs fine, so we auto-detect and add the
        # block when uname -r doesn't shout "microsoft". Override with
        # DIORAMAP_TIER_SKIP_TSAN=1 to skip explicitly (e.g. on a slow
        # runner where you want under-30-min wall).
        #
        # Full tier additionally builds the research step libs (PolyFit /
        # CGAL-KSR / Roofer) so they get the same warning + audit coverage
        # as v7. The fast tier leaves them off (default DIORAMAP_RESEARCH=OFF
        # since the v7/research split in 2d032d4) to keep the budget tight.
        # build-variant.sh honors $DIORAMAP_EXTRA_CMAKE_FLAGS.
        export DIORAMAP_EXTRA_CMAKE_FLAGS="$(tier_extra_cmake_flags full)"

        run_build release; run_ctest release
        run_build asan;    run_ctest asan
        run_integration asan   # all 10 scenarios, no filter
        run_build clang;   run_ctest clang

        # TSan: real Linux only.
        if [[ "${DIORAMAP_TIER_SKIP_TSAN:-0}" != "1" ]] \
           && [[ "$(uname -r)" != *microsoft* ]] \
           && [[ "$(uname -r)" != *WSL* ]]; then
            run_build tsan;    run_ctest tsan
        else
            printf '[tier] step %-22s :: SKIP  (WSL2 or DIORAMAP_TIER_SKIP_TSAN=1)\n' \
                "tsan"
            STEP_TIMINGS+=("0 0 tsan(skip)")
        fi

        run_build cov
        run_coverage

        # Fuzz: regression-only (replay the checked-in corpus, no new
        # exploration). Builds libFuzzer targets with -DDIORAMAP_FUZZ=ON
        # then runs each against its corpus dir via `-runs=0`. Catches
        # regressions of historically-fixed bugs (e.g. run_journal
        # parse_produced_json's 2026-05-02 realloc UAF). The 1 h/target
        # `explore` mode belongs in nightly (task #85).
        #
        # Override DIORAMAP_EXTRA_CMAKE_FLAGS for the fuzz build — the
        # tier-wide DIORAMAP_RESEARCH=ON would pull in Roofer/CGAL/PolyFit
        # (we measured 826 s wall on the QA VM) for zero added coverage:
        # the fuzz targets exercise dioramap_hash + run_journal only,
        # both of which are in the core build. Save/restore rather than
        # subshell so run_build's STEP_TIMINGS+=… and OVERALL_RC updates
        # land in the parent scope.
        _saved_extra="${DIORAMAP_EXTRA_CMAKE_FLAGS:-}"
        export DIORAMAP_EXTRA_CMAKE_FLAGS=""
        run_build fuzz
        run_fuzz regression
        export DIORAMAP_EXTRA_CMAKE_FLAGS="$_saved_extra"
        unset _saved_extra

        run_frama_c
        # Duplication detection (report-only, non-gating).
        run_cpd
        run_jscpd
        ;;
    nightly|weekly)
        echo "[tier] $TIER not yet implemented" >&2
        exit 2
        ;;
    *)
        echo "unknown tier: $TIER (try: fast | full | nightly | weekly)" >&2
        exit 2
        ;;
esac

# ── summary.json ──────────────────────────────────────────────────────
{
    echo '{'
    echo "  \"tier\": \"$TIER\","
    echo "  \"timestamp\": \"$(date -Is)\","
    echo "  \"git_sha\": \"$(git rev-parse HEAD 2>/dev/null || echo unknown)\","
    echo "  \"junit\": ["
    first=1
    for x in "$ART"/junit/*.xml; do
        [[ -f "$x" ]] || continue
        [[ $first -eq 0 ]] && echo ","
        first=0
        printf '    "%s"' "$(basename "$x")"
    done
    echo
    echo "  ],"
    echo "  \"overall_rc\": $OVERALL_RC"
    echo '}'
} > "$ART/summary.json"

# Optional: generate the Allure HTML report.  Skip if allure isn't installed
# (the JUnit XML is still produced and any consumer can pick it up).
if command -v allure >/dev/null 2>&1; then
    if allure generate "$ART/junit/" --clean -o "$ART/allure-report" >/dev/null 2>&1; then
        echo "[tier] allure report → ${ART#$REPO_ROOT/}/allure-report/index.html"
    else
        echo "[tier] allure generate failed (non-fatal)"
    fi
fi

print_step_timings

echo
if [[ $OVERALL_RC -eq 0 ]]; then
    echo "[tier] $TIER PASS — artifacts at ${ART#$REPO_ROOT/}"
else
    echo "[tier] $TIER FAIL — artifacts at ${ART#$REPO_ROOT/}"
fi
echo
echo "Open in browser:"
echo "  file://$ART/allure-report/index.html"
echo "Or serve locally:"
echo "  allure serve $ART/junit/"
exit "$OVERALL_RC"
