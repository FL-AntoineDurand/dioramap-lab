#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Orchestrator: run a single integration scenario against a chosen
# processor binary.  Exit 0 on pass, non-zero on fail.
#
# Usage:
#   run_scenario.sh <scenario_name> <binary_path>
#
# Example:
#   tests/integration/run_scenario.sh batch_demo_chain_warm \
#       build-asan/dioramap-processor
#
# The scenario script lives at tests/integration/scenarios/<name>.sh.
# It MUST source ../lib/isolation.sh and use the helpers there.

set -euo pipefail

usage() {
    cat <<EOF >&2
usage: $(basename "$0") <scenario_name> <binary_path>

scenario_name : a script under tests/integration/scenarios/<name>.sh
binary_path   : path to dioramap-processor (or its variant)

example:
  $(basename "$0") batch_demo_chain_warm build-asan/dioramap-processor

Available scenarios:
EOF
    local here
    here="$(cd "$(dirname "$0")" && pwd)"
    for s in "$here/scenarios"/*.sh; do
        [[ -f "$s" ]] || continue
        local n
        n=$(basename "$s" .sh)
        echo "  $n" >&2
    done
    exit 2
}

[[ $# -eq 2 ]] || usage

SCEN_NAME="$1"
SCEN_BIN_RAW="$2"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCEN_SCRIPT="$SCRIPT_DIR/scenarios/${SCEN_NAME}.sh"

[[ -f "$SCEN_SCRIPT" ]] || {
    echo "ERROR: no scenario '$SCEN_NAME' (looked at $SCEN_SCRIPT)" >&2
    usage
}

# Resolve binary to an absolute path so the scenario can chdir without
# losing it.
if [[ ! -x "$SCEN_BIN_RAW" ]]; then
    echo "ERROR: binary not found or not executable: $SCEN_BIN_RAW" >&2
    exit 2
fi
SCEN_BIN="$(cd "$(dirname "$SCEN_BIN_RAW")" && pwd)/$(basename "$SCEN_BIN_RAW")"
export SCEN_BIN

# Useful for scenarios that need fixture paths (e.g. pipeline JSONs).
# Honour pre-set env vars so a packaged bundle (e.g. shipped to prod
# for TSan validation, see docs/quality/README.md + the
# bundling section) can point at its own pipelines/ dir without the
# in-repo layout.
SCEN_REPO_ROOT="${SCEN_REPO_ROOT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
export SCEN_REPO_ROOT
SCEN_PIPELINES_DIR="${SCEN_PIPELINES_DIR:-$SCEN_REPO_ROOT/v3/pipelines}"
export SCEN_PIPELINES_DIR
SCEN_LIB_DIR="$SCRIPT_DIR/lib"
export SCEN_LIB_DIR
SCEN_CLIENTS_DIR="$SCRIPT_DIR/clients"
export SCEN_CLIENTS_DIR

echo "[orchestrator] scenario=$SCEN_NAME binary=$SCEN_BIN"

# Run the scenario in a subshell so its `set -e` and traps don't leak.
# The scenario's own EXIT trap (set in isolation_init) handles cleanup.
exec bash "$SCEN_SCRIPT"
