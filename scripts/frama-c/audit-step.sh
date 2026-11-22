#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Audit one steps/<dir>/ directory through Frama-C. Most step modules have
# no externally callable harness — they're plugged into the runner by
# step_register(). We parse all .c files in the directory, drive Eva with
# -lib-entry per public function discovered via metrics, and dump alarms.
#
# Usage: scripts/frama-c/audit-step.sh <step-dir-name>
#   e.g. scripts/frama-c/audit-step.sh classification
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

STEP="${1:?step dir required}"
SRC_DIR="steps/$STEP"
if [[ ! -d "$SRC_DIR" ]]; then
    echo "no such steps/ dir: $STEP" >&2; exit 2
fi
SOURCES=( $(find "$SRC_DIR" -maxdepth 1 -name '*.c') )
if [[ ${#SOURCES[@]} -eq 0 ]]; then
    echo "no .c files in $SRC_DIR" >&2; exit 2
fi

OUT=".frama-c-out/step-$STEP"
mkdir -p "$OUT"

GCC_INC="$(gcc -print-file-name=include)"
CPP_EXTRA="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DDIORAMAP_FRAMA_C_AUDIT=1 -U__SIZEOF_INT128__ -Iscripts/frama-c/stubs -Icore/include -Isteps/$STEP -Isteps/buildings -Isteps/classification -Isteps/coords -Isteps/copc -Isteps/demo -Isteps/fetchers -Isteps/gpu-smoke -Isteps/ground -Isteps/ksr -Isteps/normals -Isteps/ortho -Isteps/polyfit -Isteps/registration -Isteps/roofer -Isteps/shadow -Isteps/v7 -I${GCC_INC} -I/usr/include -I/usr/include/x86_64-linux-gnu"

# Steps have no main(). Eva needs an entry point to run; without one we
# fall back to parse + RTE annotation generation, which still produces
# CSV-able properties — Eva-unverified, but the annotations themselves
# catch dtype / OOB / signed-overflow callout sites we can spot-check.
# We pick the first step_def's exec function as the entry when we find
# one — otherwise drop to lib-entry on all externs.
ENTRY=""
ENTRY_FN=$(grep -hE 'step_def_t [a-zA-Z0-9_]+_def *=' "${SOURCES[@]}" 2>/dev/null \
    | head -1 \
    | sed -E 's/.*step_def_t ([a-zA-Z0-9_]+)_def.*/\1/')
if [[ -n "$ENTRY_FN" ]]; then
    EXEC_FN=$(grep -hE '\.exec *=[[:space:]]*[a-zA-Z0-9_]+' "${SOURCES[@]}" 2>/dev/null \
        | grep -v exec_fn \
        | head -1 \
        | sed -E 's/.*\.exec *=[[:space:]]*([a-zA-Z0-9_]+).*/\1/')
    if [[ -n "$EXEC_FN" ]]; then
        ENTRY="-main $EXEC_FN"
    fi
fi

set +e
if [[ -n "$ENTRY" ]]; then
    frama-c \
        -machdep x86_64 $(frama-c -c11 /dev/null >/dev/null 2>&1 && echo -c11) \
        -cpp-extra-args="$CPP_EXTRA" \
        "${SOURCES[@]}" \
        -lib-entry $ENTRY \
        -rte \
        -then -eva -eva-precision 1 \
        -then -report -report-csv "$OUT/alarms.csv" \
        > "$OUT/eva.log" 2>&1
else
    # parse + RTE only (no Eva)
    frama-c \
        -machdep x86_64 $(frama-c -c11 /dev/null >/dev/null 2>&1 && echo -c11) \
        -cpp-extra-args="$CPP_EXTRA" \
        "${SOURCES[@]}" \
        -rte \
        -then -metrics \
        > "$OUT/eva.log" 2>&1
fi
RC=$?
set -e

if [[ -f "$OUT/alarms.csv" ]]; then
    awk -F'\t' '
        NR==1 { print; next }
        $6 != "Considered valid" && $6 != "Completely validated" &&
        $6 != "Valid" && $6 != "Valid under hypotheses" && $6 != "Reachable" &&
        $6 != "Dead" && $6 != ""
    ' "$OUT/alarms.csv" > "$OUT/unproven.tsv"

    awk -F'\t' 'NR==1 || $1 ~ /^steps/' "$OUT/alarms.csv" > "$OUT/alarms-ours.tsv"
fi

echo "==== step $STEP ===="
echo "  sources: ${#SOURCES[@]} files"
echo "  exit:    $RC"
if [[ -f "$OUT/eva.log" ]]; then
    grep -E 'alarms generated|coverage|errors and warnings' "$OUT/eva.log" | head -5 | sed 's/^/  /'
fi
if [[ -f "$OUT/unproven.tsv" ]]; then
    UNPROVEN=$(($(wc -l < "$OUT/unproven.tsv") - 1))
    OURS_UNPROVEN=$(awk -F'\t' 'NR>1 && $1 ~ /^steps/ && $6 != "Valid" && $6 != "Dead" && $6 != "Considered valid" && $6 != "Completely validated"' "$OUT/alarms-ours.tsv" 2>/dev/null | wc -l)
    echo "  unproven (total):       $UNPROVEN"
    echo "  unproven (steps/ only): $OURS_UNPROVEN"
fi
exit "$RC"
