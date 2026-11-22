#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Per-scenario isolation primitives.
#
# Every integration scenario MUST source this file and use the helpers
# below for ports, cache dirs, log files, and output dirs.  Direct use
# of /tmp/dioramap-cache, /tmp/dioramap-processor.log, or any
# well-known port (7788, 7801, 8080, etc.) is FORBIDDEN — the matrix
# may run on a shared dev/prod box and must never collide with
# whatever else is running there.
#
# Contract:
#   - All paths under one $SCEN_TMPDIR per scenario invocation.
#   - All ports allocated from the ephemeral range via the kernel
#     (Python socket bind to port 0 → kernel assigns a free port).
#   - Cleanup on EXIT removes $SCEN_TMPDIR EXCEPT when SCEN_KEEP=1
#     (set automatically on failure for triage).
#
# Environment variables this file sets:
#   SCEN_NAME       — caller-set; used in tmpdir prefix
#   SCEN_TMPDIR     — root for everything this scenario writes
#   SCEN_CACHE_DIR  — pass via --cache-dir
#   SCEN_OUTPUT_DIR — pass via --output-dir (batch mode)
#   SCEN_LOG        — processor stderr redirect target
#   SCEN_BIN        — processor binary path (caller-supplied)
#
# Functions:
#   isolation_init <scenario_name>  — set up tmpdir, traps
#   alloc_port                      — print a free TCP port to stdout
#   start_processor_serve <args...> — launch processor in serve mode,
#                                     wait for port, capture pid
#   stop_processor                  — TERM + wait + KILL fallback
#   require <bin> [<bin> ...]       — assert each command is on PATH
#   fail <msg>                      — print, set SCEN_KEEP=1, exit 1
#   pass                            — print OK, exit 0

set -euo pipefail

# ─── lifecycle ────────────────────────────────────────────────────────

isolation_init() {
    SCEN_NAME="${1:?usage: isolation_init <name>}"
    # Each scenario gets its own tmpdir under /tmp/dioramap-it-<random>
    # so two parallel scenarios cannot collide and so a stray one is
    # easy to identify (`ls /tmp/dioramap-it-*`).
    SCEN_TMPDIR=$(mktemp -d -t "dioramap-it-${SCEN_NAME}.XXXXXXXX")
    SCEN_CACHE_DIR="$SCEN_TMPDIR/cache"
    SCEN_OUTPUT_DIR="$SCEN_TMPDIR/output"
    SCEN_LOG="$SCEN_TMPDIR/processor.log"
    mkdir -p "$SCEN_CACHE_DIR" "$SCEN_OUTPUT_DIR"
    SCEN_PID=""
    SCEN_KEEP=0

    # Cleanup trap — runs whether the scenario passed, failed, or was
    # interrupted.  Preserves the tmpdir on failure for triage.
    trap '_isolation_cleanup' EXIT INT TERM

    echo "[isolation] scenario=$SCEN_NAME tmpdir=$SCEN_TMPDIR" >&2
}

_isolation_cleanup() {
    local rc=$?
    # If we have a running processor, stop it.
    if [[ -n "${SCEN_PID:-}" ]]; then
        stop_processor 2>/dev/null || true
    fi
    if [[ $rc -ne 0 || "${SCEN_KEEP:-0}" == "1" ]]; then
        echo "[isolation] preserving $SCEN_TMPDIR for triage (rc=$rc)" >&2
    else
        rm -rf "$SCEN_TMPDIR"
    fi
}

# ─── port allocation ──────────────────────────────────────────────────
#
# Bind to port 0 → kernel returns an ephemeral free port.  Small race
# window between socket close and processor bind, but in practice
# negligible and the processor would just fail to start (caught by
# start_processor_serve's wait_for_port).

alloc_port() {
    if ! command -v python3 >/dev/null 2>&1; then
        echo "[isolation] FAIL: python3 required for port allocation" >&2
        return 1
    fi
    python3 -c '
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
'
}

# ─── processor lifecycle (serve mode) ─────────────────────────────────

# start_processor_serve <port> <extra args...>
# Launches the processor in serve mode bound to localhost only,
# captures stderr to $SCEN_LOG, sets $SCEN_PID, waits up to 10s for
# the port to accept connections.  Aborts the scenario on any failure.
start_processor_serve() {
    local port="${1:?usage: start_processor_serve <port> [args...]}"
    shift
    if [[ -z "${SCEN_BIN:-}" ]]; then
        fail "SCEN_BIN must be set before start_processor_serve"
    fi
    # Note: serve mode binds all interfaces (no --iface arg today).
    # Isolation comes from the unique ephemeral port + tmp cache dir.
    # If serve grows --iface, add it here pinned to 127.0.0.1.
    #
    # VALGRIND_WRAPPER (set by run_scenario_under.sh) gets prepended
    # so the same scenarios run unchanged under valgrind.  Empty in
    # the normal case.  Use eval to expand the wrapper string into
    # argv tokens.
    if [[ -n "${VALGRIND_WRAPPER:-}" ]]; then
        DIORAMAP_CACHE_DIR="$SCEN_CACHE_DIR" \
        DIORAMAP_LOG_LEVEL="${DIORAMAP_LOG_LEVEL:-info}" \
            eval "$VALGRIND_WRAPPER \"\$SCEN_BIN\" serve --port \$port --cache-dir \"\$SCEN_CACHE_DIR\" $*" \
            >"$SCEN_LOG" 2>&1 &
    else
        DIORAMAP_CACHE_DIR="$SCEN_CACHE_DIR" \
        DIORAMAP_LOG_LEVEL="${DIORAMAP_LOG_LEVEL:-info}" \
            "$SCEN_BIN" serve --port "$port" --cache-dir "$SCEN_CACHE_DIR" "$@" \
            >"$SCEN_LOG" 2>&1 &
    fi
    SCEN_PID=$!
    SCEN_PORT=$port

    # Wait for the port to accept connections (or processor to die).
    # Valgrind launch is much slower; allow proportionally more time.
    local timeout_units=100
    if [[ -n "${VALGRIND_WRAPPER:-}" ]]; then
        timeout_units=600
    fi
    local waited=0
    while (( waited < timeout_units )); do
        if ! kill -0 "$SCEN_PID" 2>/dev/null; then
            fail "processor died during startup; see $SCEN_LOG"
        fi
        if (echo > /dev/tcp/127.0.0.1/$port) 2>/dev/null; then
            echo "[isolation] processor ready pid=$SCEN_PID port=$port" >&2
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    local secs=$(( timeout_units / 10 ))
    fail "processor did not start on :$port within ${secs}s"
}

# stop_processor — graceful TERM, then KILL after 5s.
stop_processor() {
    if [[ -z "${SCEN_PID:-}" ]]; then
        return 0
    fi
    if kill -0 "$SCEN_PID" 2>/dev/null; then
        kill -TERM "$SCEN_PID" 2>/dev/null || true
        local waited=0
        while (( waited < 50 )) && kill -0 "$SCEN_PID" 2>/dev/null; do
            sleep 0.1
            waited=$((waited + 1))
        done
        if kill -0 "$SCEN_PID" 2>/dev/null; then
            echo "[isolation] processor pid=$SCEN_PID didn't TERM; KILLing" >&2
            kill -KILL "$SCEN_PID" 2>/dev/null || true
        fi
    fi
    wait "$SCEN_PID" 2>/dev/null || true
    SCEN_PID=""
}

# ─── prerequisites + result ───────────────────────────────────────────

require() {
    local missing=0
    for cmd in "$@"; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            echo "[isolation] FAIL: required command '$cmd' not on PATH" >&2
            missing=1
        fi
    done
    (( missing == 0 )) || exit 2
}

fail() {
    SCEN_KEEP=1
    echo "[$SCEN_NAME] FAIL: $*" >&2
    exit 1
}

pass() {
    echo "[$SCEN_NAME] OK ${1:-}" >&2
    exit 0
}

# ─── sanitizer report detection ───────────────────────────────────────
#
# After the scenario, scan $SCEN_LOG for sanitizer reports.  Any match
# fails the scenario (the binary may have aborted cleanly but ASan/TSan
# wrote a report before exit).

assert_no_sanitizer_reports() {
    if grep -qE 'AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|double free|heap-use-after-free|data race' "$SCEN_LOG"; then
        echo "[$SCEN_NAME] sanitizer report in $SCEN_LOG:" >&2
        grep -A5 -E 'Sanitizer|double free|use-after-free|data race' "$SCEN_LOG" | head -40 >&2
        fail "sanitizer report"
    fi
}
