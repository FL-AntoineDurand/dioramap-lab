#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Run Frama-C against one module of the v3 source tree.
#
# Usage:
#   scripts/frama-c/audit-module.sh <module-key>      # uses modules.tsv
#   scripts/frama-c/audit-module.sh --sources <src1> [<src2>...] --harness <h.c> --key <k>
#
# Output:
#   .frama-c-out/<key>/parse.log    parser output
#   .frama-c-out/<key>/eva.log      Eva analysis output
#   .frama-c-out/<key>/alarms.csv   raw alarms via -report-csv
#   .frama-c-out/<key>/unproven.tsv filtered unproven properties
#   .frama-c-out/<key>/summary.txt  one-screen digest
#
# Curated findings still need to be hand-written into
# the per-module ACSL contracts in core/include/v3/ — this script only produces the
# evidence.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

GCC_INC="$(gcc -print-file-name=include)"

# Default flags every module gets. Frama-C does not need a successful
# link, just preprocess + parse. Frama-C's own libc replaces system
# <stdio.h>, <string.h>, <pthread.h>, <stdlib.h>, etc. with ACSL-spec'd
# versions; we add system include dirs only for project + 3rd-party
# headers (xxhash, jansson, proj, sqlite3, libwebsockets, curl, ssl).
COMMON_DEFINES=(
    "-D_POSIX_C_SOURCE=200809L"
    "-D_DEFAULT_SOURCE"
    "-DDIORAMAP_FRAMA_C_AUDIT=1"
    "-U__SIZEOF_INT128__"     # Frama-C 25 can't parse __int128 typedefs in <linux/types.h>
)

COMMON_INCLUDES=(
    "-Iscripts/frama-c/stubs"   # shadows xxhash.h, etc.
    "-Icore/include"
    "-Iscripts/frama-c"         # for harness includes
    "-Isteps/buildings" "-Isteps/classification" "-Isteps/coords" "-Isteps/copc"
    "-Isteps/demo" "-Isteps/fetchers" "-Isteps/gpu-smoke" "-Isteps/ground"
    "-Isteps/ksr" "-Isteps/normals" "-Isteps/ortho" "-Isteps/polyfit"
    "-Isteps/registration" "-Isteps/roofer" "-Isteps/shadow" "-Isteps/v7"
    "-I${GCC_INC}"              # gcc intrinsics (needed by some system headers)
    "-I/usr/include"            # proj.h, sqlite3.h, jansson.h, libwebsockets.h, openssl/
    "-I/usr/include/x86_64-linux-gnu"  # curl/curl.h, asm/types.h (multi-arch)
    # Frama-C's libc shadows <stdio.h>, <pthread.h>, etc. via FRAMAC_SHARE.
)

usage() {
    cat <<EOF
Usage:
  $0 <module-key>
  $0 --key <k> --sources <s1> [<s2>...] --harness <h.c> [--no-harness]

Module keys are listed in scripts/frama-c/modules.tsv.
EOF
}

# ----- parse args -----
KEY=""
SOURCES=()
HARNESS=""
USE_HARNESS=1

if [[ $# -eq 0 ]]; then usage; exit 2; fi

if [[ "$1" != --* ]]; then
    KEY="$1"
    # look up in modules.tsv
    row="$(awk -F'\t' -v k="$KEY" '$1 == k {print}' scripts/frama-c/modules.tsv)"
    if [[ -z "$row" ]]; then
        echo "module key not found in modules.tsv: $KEY" >&2; exit 2
    fi
    IFS=$'\t' read -r _ label sources harness <<< "$row"
    # shellcheck disable=SC2206
    SOURCES=($sources)
    HARNESS="$harness"
    LABEL="$label"
else
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --key) KEY="$2"; shift 2;;
            --sources) shift; while [[ $# -gt 0 && "$1" != --* ]]; do SOURCES+=("$1"); shift; done;;
            --harness) HARNESS="$2"; shift 2;;
            --no-harness) USE_HARNESS=0; shift;;
            *) echo "unknown flag: $1" >&2; usage; exit 2;;
        esac
    done
fi

if [[ -z "$KEY" || ${#SOURCES[@]} -eq 0 ]]; then usage; exit 2; fi

OUTDIR=".frama-c-out/$KEY"
mkdir -p "$OUTDIR"

FRAMAC_ARGS=(
    -machdep x86_64
    $(frama-c -c11 /dev/null >/dev/null 2>&1 && echo -c11)
    # We use INFINITY as a sentinel value in many places (min/max trackers,
    # residual arrays, etc).  RTE's default -warn-special-float=non-finite
    # would flag every `x = INFINITY` as an `is_nan_or_infinite` invalid
    # alarm.  Keep NaN detection (NaN appearing accidentally IS a real bug)
    # but accept infinities.
    -warn-special-float nan
    -cpp-extra-args="${COMMON_DEFINES[*]} ${COMMON_INCLUDES[*]}"
)

# Input files
INPUTS=("${SOURCES[@]}")
if [[ $USE_HARNESS -eq 1 && -n "$HARNESS" && -f "$HARNESS" ]]; then
    INPUTS+=("$HARNESS")
    MAIN_ARG=(-main main)
else
    # No harness — let Eva pick the first non-static function with -lib-entry.
    # Caller of the runner can override with --main env var if needed.
    MAIN_ARG=(-lib-entry)
    if [[ -n "${AUDIT_MAIN:-}" ]]; then
        MAIN_ARG=(-lib-entry -main "$AUDIT_MAIN")
    fi
fi

echo ""
echo "==== Frama-C audit: $KEY ===="
echo "sources: ${SOURCES[*]}"
echo "harness: ${HARNESS:-<none>}"
echo "out:     $OUTDIR/"
echo ""

# Parse + Eva + RTE + report all in one Frama-C invocation; keeps the
# parsed AST in memory between phases.
#
# eva-precision is normally 2 (Frama-C's default for "most use cases").
# Big iterative modules (notably v7_br_impl, ~3500 lines of impl + 5
# named ops post make-shared cascade) push the per-property cost up
# to a 25-min single-module wall on the QA VM — that's task #88. The
# AUDIT_EVA_PRECISION env var lets the orchestrator (run-frama-c.sh)
# override per-module without touching this script: e.g.
#     AUDIT_EVA_PRECISION=1 audit-module.sh v7_br_impl
# at the cost of weakening some "Unknown → Valid" classifications.
# Leave it unset for the precision-2 default (which is what every
# small + medium module gets and what the gate baseline assumes).
EVA_PRECISION="${AUDIT_EVA_PRECISION:-2}"

set +e
frama-c "${FRAMAC_ARGS[@]}" "${INPUTS[@]}" \
    "${MAIN_ARG[@]}" \
    -rte \
    -then -eva -eva-precision "$EVA_PRECISION" -eva-warn-key locals-escaping=inactive \
    -then -report -report-csv "$OUTDIR/alarms.csv" \
    > "$OUTDIR/eva.log" 2>&1
RC=$?
set -e

# Filter unproven properties — anything not "Considered valid" / "Valid" /
# "Completely validated" / "Reachable" / "Dead" is a real alarm.
# (Dead == unreachable code, not an alarm.)
awk -F'\t' '
    NR==1 { print; next }
    $6 != "Considered valid" &&
    $6 != "Completely validated" &&
    $6 != "Valid" &&
    $6 != "Valid under hypotheses" &&
    $6 != "Reachable" &&
    $6 != "Dead" &&
    $6 != ""
' "$OUTDIR/alarms.csv" > "$OUTDIR/unproven.tsv"

# Also filter alarms that point into our own source tree (not FRAMAC_SHARE,
# not the audit stubs/harnesses).
awk -F'\t' '
    NR==1 || $1 ~ /^(core|steps)/
' "$OUTDIR/alarms.csv" > "$OUTDIR/alarms-ours.tsv"

# Summary digest.
{
    echo "Module: $KEY  (${LABEL:-})"
    echo "Sources: ${SOURCES[*]}"
    echo "Harness: ${HARNESS:-<none>}"
    echo "Frama-C exit: $RC"
    echo ""
    echo "---- Eva summary line ----"
    grep -E 'alarms generated|statements reached|coverage|errors and warnings' "$OUTDIR/eva.log" \
        | head -20
    echo ""
    echo "---- Unproven properties (count): $(($(wc -l < "$OUTDIR/unproven.tsv") - 1)) ----"
    head -50 "$OUTDIR/unproven.tsv"
    echo ""
    echo "---- Alarms in our source: $(($(wc -l < "$OUTDIR/alarms-ours.tsv") - 1)) total ----"
    # show only the ones that aren't trivially Valid
    awk -F'\t' 'NR==1 || ($6 != "Valid" && $6 != "Dead" && $6 != "Considered valid" && $6 != "Completely validated")' \
        "$OUTDIR/alarms-ours.tsv" | head -50
} > "$OUTDIR/summary.txt"

cat "$OUTDIR/summary.txt"
echo ""
echo "Full log: $OUTDIR/eva.log"

exit "$RC"
