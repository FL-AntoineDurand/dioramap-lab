#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
//
// Shared WebSocket client for integration scenarios.
//
// Usage from a scenario script:
//
//   node tests/integration/clients/node-ws.js \
//       --url ws://127.0.0.1:$PORT/?key=... \
//       --command run \
//       --pipeline demo-chain \
//       --input '{"base":5}' \
//       --subscribe out
//
// The script exits 0 on success and prints a single JSON object to
// stdout describing what happened (for the scenario to assert on).
// Exit non-0 on any failure (timeout, error frame, etc.).
//
// Commands:
//   run               — send RUN, await RUN_COMPLETE; print {steps_executed,cache_hits}
//   subscribe         — send SUBSCRIBE, await SUBSCRIBE_ACK
//   cancel-after      — send RUN, wait Nms, send CANCEL; print whether ERROR was received
//   close-after       — send RUN, wait Nms, close socket abruptly
//   reconnect         — connect, send token (if any), then send a follow-up RUN
//   parallel-runs     — open N sockets, send identical RUN on each, await all completes
//   burst             — send N sequential RUNs on one socket (cache-hit class)
//
// All commands time out after --timeout-ms (default 30000).

'use strict';

const WebSocket = require('ws');
const { parseArgs } = require('node:util');

const { values: args } = parseArgs({
    options: {
        url:           { type: 'string' },
        command:       { type: 'string' },
        pipeline:      { type: 'string' },
        input:         { type: 'string', default: '{}' },
        subscribe:     { type: 'string', default: '' },   // comma-separated
        artifact:      { type: 'string' },                 // for SUBSCRIBE
        'cancel-after-ms': { type: 'string', default: '50' },
        'close-after-ms':  { type: 'string', default: '50' },
        n:             { type: 'string', default: '10' },
        'timeout-ms':  { type: 'string', default: '30000' },
        verbose:       { type: 'boolean', default: false },
    },
    strict: true,
});

function log(msg) { if (args.verbose) console.error('[client]', msg); }

function fatal(msg) {
    console.error('[client] FAIL:', msg);
    process.exit(1);
}

function emit(obj) {
    process.stdout.write(JSON.stringify(obj) + '\n');
}

function parseSubscribe(s) {
    return s ? s.split(',').map(x => x.trim()).filter(Boolean) : [];
}

function makeRunMessage() {
    return {
        t: 'RUN',
        pipeline: args.pipeline,
        input: JSON.parse(args.input),
        subscribe: parseSubscribe(args.subscribe),
    };
}

// ─── single-socket helpers ────────────────────────────────────────────
//
// openSocket attaches a persistent 'message' listener BEFORE returning.
// All inbound text frames are parsed and pushed onto ws._frameQueue;
// awaiters (awaitFrame) consume from the queue first, then wait for
// new frames.  Without this, frames that arrive between 'open' firing
// and the caller attaching their own listener are silently dropped.
// (Symptom: SESSION_STARTED arriving in the same tick as the WS
// upgrade — common on localhost — would never reach awaitFrame.)

function openSocket(url, timeoutMs) {
    return new Promise((resolve, reject) => {
        const ws = new WebSocket(url);
        ws._frameQueue = [];        // text frames as parsed objects
        ws._frameWaiters = [];      // [{ wanted, resolve, reject, timer }]
        ws._closed = false;
        ws._binaryCount = 0;        // count of binary frames received
                                    //   (no parsing — used by rehydrate)

        const drain = () => {
            // Try to satisfy each waiter against the head of the queue.
            // A waiter that doesn't match the next frame stays put;
            // unmatched frames at the head are discarded only when a
            // waiter rejects them via the wanted-list check.
            //
            // Simpler model: scan the queue for any frame matching any
            // waiter, oldest-first.  Drop unmatched frames? No — we
            // want to keep them so a later awaiter can pick them up.
            // Trade-off: queue grows unboundedly if frames pile up
            // unread.  For our tests this is bounded.
            for (let qi = 0; qi < ws._frameQueue.length; ) {
                const msg = ws._frameQueue[qi];
                let matched = false;
                for (let wi = 0; wi < ws._frameWaiters.length; wi++) {
                    const w = ws._frameWaiters[wi];
                    if (w.wanted.includes(msg.t)) {
                        clearTimeout(w.timer);
                        w.resolve(msg);
                        ws._frameWaiters.splice(wi, 1);
                        ws._frameQueue.splice(qi, 1);
                        matched = true;
                        break;
                    }
                    // Special: ERROR fails any waiter that hasn't asked
                    // for ERROR specifically.
                    if (msg.t === 'ERROR' && !w.wanted.includes('ERROR')) {
                        clearTimeout(w.timer);
                        w.reject(new Error('ERROR frame: ' + JSON.stringify(msg)));
                        ws._frameWaiters.splice(wi, 1);
                        ws._frameQueue.splice(qi, 1);
                        matched = true;
                        break;
                    }
                }
                if (!matched) qi++;
            }
        };

        ws.on('message', (data, isBinary) => {
            if (isBinary) { ws._binaryCount++; return; }
            try {
                const msg = JSON.parse(data);
                ws._frameQueue.push(msg);
                drain();
            } catch (e) { /* malformed text — drop */ }
        });

        ws.on('close', () => {
            ws._closed = true;
            for (const w of ws._frameWaiters) {
                clearTimeout(w.timer);
                w.reject(new Error('socket closed before ' + w.wanted.join('|')));
            }
            ws._frameWaiters = [];
        });

        ws._drain = drain;

        const t = setTimeout(() => { ws.close(); reject(new Error('connect timeout')); }, timeoutMs);
        ws.on('open', () => { clearTimeout(t); resolve(ws); });
        ws.on('error', (e) => { clearTimeout(t); reject(e); });
    });
}

// Awaits a frame with msg.t === expectedType (or any of expected[]).
// Pulls from ws._frameQueue if a match is already buffered; else waits.
function awaitFrame(ws, expected, timeoutMs) {
    const wanted = Array.isArray(expected) ? expected : [expected];
    return new Promise((resolve, reject) => {
        if (ws._closed) return reject(new Error('socket closed'));
        const w = { wanted, resolve, reject, timer: null };
        w.timer = setTimeout(() => {
            const i = ws._frameWaiters.indexOf(w);
            if (i >= 0) ws._frameWaiters.splice(i, 1);
            reject(new Error(`timeout awaiting ${wanted.join('|')}`));
        }, timeoutMs);
        ws._frameWaiters.push(w);
        ws._drain();
    });
}

function sleep(ms) {
    return new Promise(r => setTimeout(r, ms));
}

// ─── command implementations ──────────────────────────────────────────

async function cmdRun() {
    const ws = await openSocket(args.url, +args['timeout-ms']);
    // SESSION_STARTED arrives unsolicited; consume but don't require.
    awaitFrame(ws, 'SESSION_STARTED', 1000).catch(() => {});
    ws.send(JSON.stringify(makeRunMessage()));
    const done = await awaitFrame(ws, 'RUN_COMPLETE', +args['timeout-ms']);
    ws.close();
    emit({
        ok: true,
        steps_executed: done.summary?.steps_executed ?? 0,
        cache_hits:     done.summary?.cache_hits ?? 0,
        duration_us:    done.summary?.duration_us ?? 0,
    });
}

async function cmdSubscribe() {
    const ws = await openSocket(args.url, +args['timeout-ms']);
    awaitFrame(ws, 'SESSION_STARTED', 1000).catch(() => {});
    if (!args.artifact) fatal('--artifact required for subscribe');
    ws.send(JSON.stringify({ t: 'SUBSCRIBE', artifact: args.artifact }));
    const ack = await awaitFrame(ws, 'SUBSCRIBE_ACK', +args['timeout-ms']);
    ws.close();
    emit({ ok: true, artifact: ack.artifact, cache_hits: ack.cache_hits });
}

async function cmdCancelAfter() {
    const ws = await openSocket(args.url, +args['timeout-ms']);
    awaitFrame(ws, 'SESSION_STARTED', 1000).catch(() => {});
    ws.send(JSON.stringify(makeRunMessage()));
    await sleep(+args['cancel-after-ms']);
    ws.send(JSON.stringify({ t: 'CANCEL' }));
    // After CANCEL we expect either ERROR (cancelled) or RUN_COMPLETE
    // (race: run finished before cancel reached).  Both are acceptable
    // outcomes; we just need the processor to stay up.
    // Wait for either RUN_COMPLETE (race won by run) or ERROR
    // (cancel landed first).  awaitFrame handles ERROR specially —
    // it rejects on ERROR unless ERROR is in the wanted list, so
    // include both.
    let outcome = 'unknown';
    try {
        const m = await awaitFrame(ws, ['RUN_COMPLETE', 'ERROR'], +args['timeout-ms']);
        outcome = m.t === 'RUN_COMPLETE' ? 'complete' : 'error';
    } catch (e) {
        if (e.message.includes('socket closed')) outcome = 'closed';
        else throw e;
    }
    ws.close();
    emit({ ok: true, outcome });
}

async function cmdCloseAfter() {
    const ws = await openSocket(args.url, +args['timeout-ms']);
    awaitFrame(ws, 'SESSION_STARTED', 1000).catch(() => {});
    ws.send(JSON.stringify(makeRunMessage()));
    await sleep(+args['close-after-ms']);
    ws.terminate();   // abrupt close, no FIN
    emit({ ok: true });
}

async function cmdParallelRuns() {
    const n = +args.n;
    const sockets = [];
    for (let i = 0; i < n; i++) sockets.push(await openSocket(args.url, +args['timeout-ms']));
    // Don't gate on SESSION_STARTED here — it can arrive after RUN.
    const results = await Promise.all(sockets.map(async (ws, idx) => {
        ws.send(JSON.stringify(makeRunMessage()));
        const m = await awaitFrame(ws, 'RUN_COMPLETE', +args['timeout-ms']);
        ws.close();
        return {
            i: idx,
            steps_executed: m.summary?.steps_executed ?? 0,
            cache_hits:     m.summary?.cache_hits ?? 0,
        };
    }));
    emit({ ok: true, n, results });
}

async function cmdBurst() {
    const n = +args.n;
    const ws = await openSocket(args.url, +args['timeout-ms']);
    awaitFrame(ws, 'SESSION_STARTED', 1000).catch(() => {});
    const summary = { steps_executed_total: 0, cache_hits_total: 0 };
    for (let i = 0; i < n; i++) {
        ws.send(JSON.stringify(makeRunMessage()));
        const m = await awaitFrame(ws, 'RUN_COMPLETE', +args['timeout-ms']);
        summary.steps_executed_total += m.summary?.steps_executed ?? 0;
        summary.cache_hits_total     += m.summary?.cache_hits ?? 0;
    }
    ws.close();
    emit({ ok: true, n, ...summary });
}

// ─── multi-step flows on one socket ───────────────────────────────────
//
// These commands chain multiple message exchanges on a single socket
// (or, for reconnect, across two sockets sharing a session token).
// Each captures the SESSION_STARTED frame's session_token where
// relevant.

// Open + capture the SESSION_STARTED token in one shot.  Returns
// { ws, token }.
async function openAndAwaitSession(url) {
    const ws = await openSocket(url, +args['timeout-ms']);
    const sess = await awaitFrame(ws, 'SESSION_STARTED', 5000);
    return { ws, token: sess.session_token };
}

// list-runs : RUN, then LIST_RUNS, return the run-journal rows.
async function cmdListRuns() {
    const { ws } = await openAndAwaitSession(args.url);
    const runMsg = { ...makeRunMessage(), test_case_label: 'integration-list-runs' };
    ws.send(JSON.stringify(runMsg));
    await awaitFrame(ws, 'RUN_COMPLETE', +args['timeout-ms']);
    ws.send(JSON.stringify({ t: 'LIST_RUNS', limit: 10 }));
    const resp = await awaitFrame(ws, 'LIST_RUNS_RESPONSE', +args['timeout-ms']);
    ws.close();
    emit({ ok: true, runs: resp.runs ?? [] });
}

// rehydrate : RUN with label, LIST_RUNS to find journal_id,
// REHYDRATE_RUN, await complete (which arrives interleaved with
// ARTIFACT_HEADER frames — we count those).
async function cmdRehydrate() {
    const { ws } = await openAndAwaitSession(args.url);
    const label = 'integration-rehydrate-' + Date.now();
    ws.send(JSON.stringify({ ...makeRunMessage(), test_case_label: label }));
    await awaitFrame(ws, 'RUN_COMPLETE', +args['timeout-ms']);

    ws.send(JSON.stringify({ t: 'LIST_RUNS', limit: 50 }));
    const list = await awaitFrame(ws, 'LIST_RUNS_RESPONSE', +args['timeout-ms']);
    const row = (list.runs ?? []).find(r => r.test_case_label === label);
    if (!row) fatal('rehydrate: no journal entry for label ' + label);
    const jid = row.journal_id;

    // REHYDRATE_RUN streams ARTIFACT_HEADER (+ binary follow-ups)
    // followed by REHYDRATE_COMPLETE or REHYDRATE_PARTIAL.
    // ws._binaryCount has been counting binary frames since open;
    // capture the baseline so the delta is just rehydrate's payloads.
    const binaryBefore = ws._binaryCount;
    ws.send(JSON.stringify({ t: 'REHYDRATE_RUN', journal_id: jid }));
    const result = await awaitFrame(ws,
        ['REHYDRATE_COMPLETE', 'REHYDRATE_PARTIAL'],
        +args['timeout-ms']);

    // Count ARTIFACT_HEADER frames the queue-based listener buffered.
    // They have been drained from the queue if any waiter matched
    // (none did here — we waited for REHYDRATE_*).  So scan what
    // remains in the queue + count the binary delta.
    const nHeaders = ws._frameQueue.filter(m => m.t === 'ARTIFACT_HEADER').length;
    const nBinary  = ws._binaryCount - binaryBefore;
    ws.close();
    emit({
        ok: true,
        kind: result.t,
        n_emitted: result.n_emitted ?? 0,
        n_headers: nHeaders,
        n_binary:  nBinary,
        journal_id: jid,
    });
}

// invalidate : RUN, INVALIDATE artifact, RUN again.  Returns both
// run summaries so the scenario can assert downstream re-execution.
async function cmdInvalidate() {
    const { ws } = await openAndAwaitSession(args.url);
    if (!args.artifact) fatal('--artifact required for invalidate');

    ws.send(JSON.stringify(makeRunMessage()));
    const run1 = await awaitFrame(ws, 'RUN_COMPLETE', +args['timeout-ms']);

    ws.send(JSON.stringify({ t: 'INVALIDATE', artifact: args.artifact }));
    const inv = await awaitFrame(ws, 'INVALIDATED', +args['timeout-ms']);

    ws.send(JSON.stringify(makeRunMessage()));
    const run2 = await awaitFrame(ws, 'RUN_COMPLETE', +args['timeout-ms']);

    ws.close();
    emit({
        ok: true,
        invalidated_step: inv.step,
        run1: { steps_executed: run1.summary?.steps_executed,
                cache_hits:     run1.summary?.cache_hits },
        run2: { steps_executed: run2.summary?.steps_executed,
                cache_hits:     run2.summary?.cache_hits },
    });
}

// reconnect-flow : RUN, capture token, terminate, reconnect with
// token, send a follow-up RUN.  Validates session_mgr_reattach +
// pin lifetime + grace state.
async function cmdReconnectFlow() {
    const first = await openAndAwaitSession(args.url);
    const token = first.token;
    first.ws.send(JSON.stringify(makeRunMessage()));
    const r1 = await awaitFrame(first.ws, 'RUN_COMPLETE', +args['timeout-ms']);
    // Abrupt close — do NOT send a clean close; the server's grace
    // path is what we want to exercise.
    first.ws.terminate();

    // Brief delay so the server's CLOSED handler fires before we try
    // to reattach.  Without this the manager would still see the
    // session as `attached` (wsi != NULL) and refuse the reattach.
    await sleep(200);

    const second = await openSocket(args.url, +args['timeout-ms']);
    // The fresh socket gets its own SESSION_STARTED for the ephemeral
    // session we'll discard via RECONNECT.
    awaitFrame(second, 'SESSION_STARTED', 2000).catch(() => {});

    second.send(JSON.stringify({ t: 'RECONNECT', session_token: token }));
    const reconn = await awaitFrame(second, 'RECONNECTED', +args['timeout-ms']);
    if (reconn.session_token !== token) {
        fatal('RECONNECTED with wrong token: ' + JSON.stringify(reconn));
    }

    // Follow-up RUN on the reconnected session.  Same input, so it
    // should be all cache hits (the original run's pins survived the
    // grace window).
    second.send(JSON.stringify(makeRunMessage()));
    const r2 = await awaitFrame(second, 'RUN_COMPLETE', +args['timeout-ms']);
    second.close();
    emit({
        ok: true,
        token_preserved: true,
        run1: { steps_executed: r1.summary?.steps_executed,
                cache_hits:     r1.summary?.cache_hits },
        run2: { steps_executed: r2.summary?.steps_executed,
                cache_hits:     r2.summary?.cache_hits },
    });
}

// ─── dispatch ─────────────────────────────────────────────────────────

const COMMANDS = {
    'run':             cmdRun,
    'subscribe':       cmdSubscribe,
    'cancel-after':    cmdCancelAfter,
    'close-after':     cmdCloseAfter,
    'parallel-runs':   cmdParallelRuns,
    'burst':           cmdBurst,
    'list-runs':       cmdListRuns,
    'rehydrate':       cmdRehydrate,
    'invalidate':      cmdInvalidate,
    'reconnect-flow':  cmdReconnectFlow,
};

(async () => {
    if (!args.url || !args.command) {
        console.error('usage: --url <ws-url> --command <cmd> ...');
        console.error('commands: ' + Object.keys(COMMANDS).join(', '));
        process.exit(2);
    }
    const fn = COMMANDS[args.command];
    if (!fn) fatal('unknown command: ' + args.command);
    try {
        await fn();
    } catch (e) {
        fatal((e && e.message) || String(e));
    }
})();
