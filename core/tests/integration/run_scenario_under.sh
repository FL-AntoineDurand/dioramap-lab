#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Wrap any integration scenario in a Valgrind invocation.
#
# Usage:
#   run_scenario_under.sh <tool> <scenario_name> <binary_path>
#
#   tool : memcheck | helgrind | drd
#
# Example:
#   run_scenario_under.sh memcheck batch_demo_chain_warm \
#       build/dioramap-processor
#
# Implementation: this wrapper exports VALGRIND_WRAPPER (a shell-quoted
# command line that the isolation lib's start_processor_serve will
# prepend to the binary launch).  For batch-mode scenarios that don't
# go through start_processor_serve, the wrapper script needs to run
# the binary itself under valgrind — see the BATCH_VALGRIND_WRAP
# section.
#
# Slowness:
#   memcheck  : ~10-30× slowdown.  Catches uninit reads + leaks.
#   helgrind  : ~30-100× slowdown.  Catches data races (different
#               algo than TSan; finds different things).
#   drd       : ~30-100× slowdown.  Same family as helgrind.
#
# Suppressions in tests/integration/valgrind.supp.

set -euo pipefail

usage() {
    cat <<EOF >&2
usage: $(basename "$0") <tool> <scenario_name> <binary_path>

tool : memcheck | helgrind | drd

example:
  $(basename "$0") memcheck batch_demo_chain_warm build/dioramap-processor
EOF
    exit 2
}

[[ $# -eq 3 ]] || usage

TOOL="$1"
SCEN="$2"
BIN="$3"

case "$TOOL" in
    memcheck|helgrind|drd) ;;
    *) echo "ERROR: unknown tool '$TOOL'" >&2; usage ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SUPP="$SCRIPT_DIR/valgrind.supp"

if ! command -v valgrind >/dev/null 2>&1; then
    echo "ERROR: valgrind not found (apt install valgrind)" >&2
    exit 2
fi

# Build the valgrind command prefix.  Common flags + per-tool flags.
VALGRIND_CMD=(
    valgrind
    "--tool=$TOOL"
    --error-exitcode=99
    --suppressions="$SUPP"
    --gen-suppressions=all
    --num-callers=30
    --child-silent-after-fork=yes
)
case "$TOOL" in
    memcheck)
        VALGRIND_CMD+=(--leak-check=full --show-leak-kinds=definite,possible
                       --track-origins=yes)
        ;;
    helgrind)
        VALGRIND_CMD+=(--history-level=approx)
        ;;
    drd)
        VALGRIND_CMD+=(--check-stack-var=yes)
        ;;
esac

# Export so the isolation lib + scenario picks it up.
export VALGRIND_WRAPPER="${VALGRIND_CMD[*]}"
echo "[run-under] tool=$TOOL wrapper=$VALGRIND_WRAPPER" >&2

# Hand off to the regular orchestrator.  Scenarios that need to wrap
# their own binary launches (batch mode) check $VALGRIND_WRAPPER and
# prepend it; serve mode delegates to isolation.sh's
# start_processor_serve which does the same.
exec "$SCRIPT_DIR/run_scenario.sh" "$SCEN" "$BIN"
