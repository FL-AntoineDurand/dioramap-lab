#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Run ctest on a build dir, emit JUnit XML.
#
# Usage: run-ctest.sh <build-dir> <out.xml> [<variant-name>] [<ctest-extra-args...>]
#
# ctest 3.21+ supports --output-junit natively; we just wrap it so the
# tier dispatcher gets a predictable interface.

set -euo pipefail

BUILD_DIR="${1:?usage: run-ctest.sh <build-dir> <out.xml> [variant-name] [extra-args...]}"
OUT="${2:?usage: run-ctest.sh <build-dir> <out.xml> [variant-name] [extra-args...]}"
VARIANT="${3:-ctest}"
shift 3 2>/dev/null || shift 2
EXTRA=("$@")

mkdir -p "$(dirname "$OUT")"

# `--output-junit` writes a single XML file with one testsuite covering
# every registered test; testsuite name defaults to the build dir name.
# We override classname via post-processing if we want richer grouping.
echo "[ctest] variant=$VARIANT dir=$BUILD_DIR"

# Sanitizer variants run 5-10× slower (TSan in particular). Tests that
# read with their own wall-clock timeouts — serve_full.py et al. — honour
# DIORAMAP_TEST_TIMEOUT_MULT to scale those budgets. Default = 1.0 on
# release/asan; bumped to 5.0 on tsan/cov where the instrumentation is
# heavy. Without this serve_full.py timed out mid-recv on the QA VM
# under tsan even though there were no actual races.
case "$VARIANT" in
    tsan)  export DIORAMAP_TEST_TIMEOUT_MULT="${DIORAMAP_TEST_TIMEOUT_MULT:-5.0}" ;;
    cov)   export DIORAMAP_TEST_TIMEOUT_MULT="${DIORAMAP_TEST_TIMEOUT_MULT:-2.0}" ;;
esac

set +e
ctest --test-dir "$BUILD_DIR" \
      --output-on-failure \
      --output-junit "$OUT" \
      -j "$(nproc)" \
      "${EXTRA[@]}"
rc=$?
set -e

# ctest emits <testsuite name="(empty)"> — rename to the variant so
# Allure (and any other consumer) can distinguish results between
# build variants instead of deduplicating them.
if [[ -f "$OUT" ]]; then
    sed -i "s|<testsuite name=\"(empty)\"|<testsuite name=\"ctest-${VARIANT}\"|" "$OUT"
    # Also tag each <testcase> with a classname for nicer grouping.
    sed -i "s|<testcase \(.*\)classname=\"\"|<testcase \1classname=\"ctest-${VARIANT}\"|g" "$OUT"
fi

echo "[ctest] variant=$VARIANT exit=$rc → $OUT"
exit "$rc"
