#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Regression smoke for v3/core/src/server/server.c session_journal_acc_append
# concurrency. The bug it guards against is documented at
# v3/docs/BUG-cache-hit-runner-cleanup.md.
#
# Approach: drive the local ASan'd processor with N identical RUN messages
# through a thin WS client. After the first RUN, every subsequent RUN is
# all-cache-hit — `steps_executed:0, cache_hits:N`. Every cache-hit run
# invokes session_journal_acc_append from worker threads in parallel.
# Without the lock, ASan catches the race; the processor SIGABRTs.
#
# With the lock (current state): all N runs complete cleanly, processor
# stays up, exit 0.
#
# Prerequisites:
#   - dioramap-processor built with DIORAMAP_ASAN=ON.
#   - Processor running on $PR_PORT (default 7801).
#   - A control-plane session with a valid dev key.
#
# This is an INTEGRATION test, not a unit test. The session_journal_acc_*
# functions are static-file-scoped in server.c, and extracting them to a
# unit-testable primitive is a real refactor (see BUG doc §"Regression
# test shape" for that path). Until that refactor lands, this shell
# script is the regression net.

set -euo pipefail

PR_PORT="${PR_PORT:-7801}"
TOKEN="${DIORAMAP_TOKEN:-}"
N_RUNS="${N_RUNS:-100}"
PIPELINE="${PIPELINE:-viewer-full}"
LAT="${LAT:-48.8698}"
LON="${LON:-2.3076}"
PROCESSOR_LOG="${PROCESSOR_LOG:-/tmp/dioramap-processor.log}"

if ! command -v node >/dev/null 2>&1; then
    echo "FAIL: node required (this script uses node's ws module)" >&2
    exit 1
fi
if ! (echo > /dev/tcp/127.0.0.1/$PR_PORT) 2>/dev/null; then
    echo "FAIL: processor not listening on :$PR_PORT — start it with: dioramap-processor serve --port $PR_PORT" >&2
    exit 1
fi

PROCESSOR_PID=$(ss -tlnp 2>/dev/null | grep ":$PR_PORT " | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2 || true)
echo "Driving processor pid=$PROCESSOR_PID with $N_RUNS identical RUN messages..."

WS_URL="ws://127.0.0.1:$PR_PORT/${TOKEN:+?token=$TOKEN}"
node - <<EOF
const WebSocket = require('ws');
const N_RUNS = $N_RUNS;
let runs_completed = 0;

function once_run(idx) {
    return new Promise((resolve, reject) => {
        const ws = new WebSocket('$WS_URL');
        let timeout = setTimeout(() => { ws.close(); reject(new Error('timeout')); }, 30000);
        ws.on('open', () => {
            ws.send(JSON.stringify({
                type: 'RUN',
                pipeline: '$PIPELINE',
                input: { lat: $LAT, lon: $LON },
            }));
        });
        ws.on('message', (data) => {
            try {
                const msg = JSON.parse(data);
                if (msg.type === 'RUN_COMPLETE' || msg.type === 'ERROR') {
                    clearTimeout(timeout);
                    ws.close();
                    if (msg.type === 'ERROR') reject(new Error('RUN error: ' + JSON.stringify(msg)));
                    else resolve();
                }
            } catch (e) { /* binary frames — ignore */ }
        });
        ws.on('error', (err) => { clearTimeout(timeout); reject(err); });
    });
}

(async () => {
    for (let i = 0; i < N_RUNS; i++) {
        try { await once_run(i); runs_completed++; }
        catch (e) {
            console.error('FAIL at run #' + i + ': ' + e.message);
            process.exit(2);
        }
        if ((i + 1) % 10 === 0) process.stdout.write('.');
    }
    console.log('\\n' + N_RUNS + ' runs completed');
})();
EOF

# After all runs, confirm the processor is still alive (would have died if
# ASan caught a race).
if ! (echo > /dev/tcp/127.0.0.1/$PR_PORT) 2>/dev/null; then
    echo "FAIL: processor died during the test (probably ASan caught a race)" >&2
    echo "Last 40 log lines:" >&2
    tail -40 "$PROCESSOR_LOG" >&2
    exit 3
fi

# Check the log for ASan reports just in case (would have aborted, but
# defensive in case of weird signal handling).
if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|double free|corruption' "$PROCESSOR_LOG"; then
    echo "FAIL: sanitizer report in $PROCESSOR_LOG:" >&2
    grep -A3 -E 'AddressSanitizer|UndefinedBehaviorSanitizer|double free|corruption' "$PROCESSOR_LOG" >&2
    exit 4
fi

echo "OK — processor survived $N_RUNS cache-hit runs without sanitizer reports"
