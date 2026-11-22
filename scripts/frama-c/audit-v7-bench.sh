#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Drive Frama-C against one v7 bench file (steps/v7/<bench-name>.c).
#
# Bench files are stand-alone executables produced for PHASE-5-V7-PORT.md's
# regression check against bench-v2 (the JavaScript reference). Each loads
# binary inputs from disk via a local slurp() helper, calls the impl's
# public functions in topological order, and dumps result JSON for diff
# against checked-in baselines.
#
# For the Frama-C audit, we feed the bench's main() as Eva's entry point.
# fopen / fread / ftell are already opaque externs in FRAMAC_SHARE/libc;
# Eva treats their returns as abstract bounded values (NULL or valid FILE*;
# size_t in [0, count]; long in [-1, LONG_MAX]). The bench's data-flow
# from slurp through the impl calls is then drivable end-to-end.
#
# Usage:  scripts/frama-c/audit-v7-bench.sh <bench-name>
# Output: .frama-c-out/v7-bench-<name>/{eva.log,alarms.csv,alarms-ours.tsv,unproven.tsv}

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

BENCH="${1:?bench name required (e.g. v7-boundary-corners-bench)}"
BENCH_SRC="steps/v7/${BENCH}.c"
if [[ ! -f "$BENCH_SRC" ]]; then
    echo "no such bench: $BENCH_SRC" >&2; exit 2
fi

# Look up the impl-file dep list.
DEPS=""
while IFS=$'\t' read -r name deps_field; do
    [[ "$name" == "$BENCH" ]] || continue
    DEPS="$deps_field"
    break
done < <(awk -F'\t' 'NR>1 {print}' scripts/frama-c/v7-benches.tsv)

if [[ -z "$DEPS" ]]; then
    echo "no deps listed in scripts/frama-c/v7-benches.tsv for: $BENCH" >&2; exit 2
fi

DEP_PATHS=""
for d in $DEPS; do
    DEP_PATHS="$DEP_PATHS steps/$d"
done

OUT=".frama-c-out/v7-bench-$BENCH"
mkdir -p "$OUT"

GCC_INC="$(gcc -print-file-name=include)"
CPP_EXTRA="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DDIORAMAP_FRAMA_C_AUDIT=1 -U__SIZEOF_INT128__ -U__HAVE_FLOAT128 -U__HAVE_DISTINCT_FLOAT128 -Iscripts/frama-c/stubs -Icore/include -Isteps/v7 -Isteps/normals -I${GCC_INC} -I/usr/include -I/usr/include/x86_64-linux-gnu"

# Bench-only cpp args: rename the bench's `main` so the shim can provide
# the real main with a synthesized argv. Applied via Frama-C's per-file
# cpp setting trick below.
BENCH_CPP_EXTRA="$CPP_EXTRA -Dmain=audit_bench_main"

echo ""
echo "==== Frama-C audit (bench-as-harness): $BENCH ===="
echo "  bench:  $BENCH_SRC"
echo "  deps:   $DEPS"
echo "  out:    $OUT/"
echo ""

SHIM="scripts/frama-c/v7-bench-shim.c"

# Rename main() to audit_bench_main() in a copy of the bench source so
# the shim's main() wins. Do NOT pre-preprocess (gcc -E baked the system
# stdio.h into the output, which collided with Frama-C's libc FILE shim).
# Frama-C's own cpp handles system includes via -nostdinc and its own
# FRAMAC_SHARE/libc, which is what every other module in this audit
# relies on.
RENAMED="$OUT/$(basename "$BENCH_SRC" .c).renamed.c"
sed -E '
    s/\bint[[:space:]]+main[[:space:]]*\(/int audit_bench_main(/g
' "$BENCH_SRC" > "$RENAMED"
if [[ ! -s "$RENAMED" ]]; then
    echo "rename failed" >&2; exit 1
fi

set +e
frama-c \
    -machdep x86_64 $(frama-c -c11 /dev/null >/dev/null 2>&1 && echo -c11) \
    -warn-special-float nan \
    -cpp-extra-args="$CPP_EXTRA" \
    "$SHIM" "$RENAMED" $DEP_PATHS \
    -main main \
    -rte \
    -then -eva -eva-precision 2 \
    -then -report -report-csv "$OUT/alarms.csv" \
    > "$OUT/eva.log" 2>&1
RC=$?
set -e

if [[ -f "$OUT/alarms.csv" ]]; then
    awk -F'\t' '
        NR==1 { print; next }
        $6 != "Considered valid" && $6 != "Completely validated" &&
        $6 != "Valid" && $6 != "Valid under hypotheses" && $6 != "Reachable" &&
        $6 != "Dead" && $6 != ""
    ' "$OUT/alarms.csv" > "$OUT/unproven.tsv"
    awk -F'\t' 'NR==1 || $1 ~ /^steps\/v7/' "$OUT/alarms.csv" > "$OUT/alarms-ours.tsv"
fi

echo "  exit:   $RC"
if [[ -f "$OUT/eva.log" ]]; then
    grep -E 'alarms generated|coverage|errors and warnings' "$OUT/eva.log" | head -5 | sed 's/^/  /'
fi
if [[ -f "$OUT/unproven.tsv" ]]; then
    UNPROVEN_TOTAL=$(($(wc -l < "$OUT/unproven.tsv") - 1))
    OURS_UNPROVEN=$(awk -F'\t' 'NR>1 && $1 ~ /^steps\/v7/ && $6 != "Valid" && $6 != "Dead" && $6 != "Considered valid" && $6 != "Completely validated"' "$OUT/alarms-ours.tsv" 2>/dev/null | wc -l)
    echo "  unproven (total):       $UNPROVEN_TOTAL"
    echo "  unproven (steps/v7):    $OURS_UNPROVEN"
fi
exit "$RC"
