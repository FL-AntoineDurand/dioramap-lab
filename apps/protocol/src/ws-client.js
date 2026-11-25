// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// WS client — L1 slice.
//
// Responsibilities shipped here:
//   - single WebSocket to dioramap-processor with the `dioramap-v1` sub-protocol
//   - text-frame decode → typed events on an EventTarget-style bus
//   - binary-frame pairing: the preceding ARTIFACT_HEADER carries {size, hash},
//     and the next binary frame is its payload (§8.1 of UNIFIED-ARCHITECTURE.md).
//     Pairing is implemented here so every subsystem above sees a single
//     `artifact` event with bytes + header already joined. Rendering is L2.
//
// Out of scope for L1: STREAM_CHUNK (B9), RECONNECT grace flow (lands with L4
// when the UI can meaningfully resume), CANCEL.

export class WSClient extends EventTarget {
  constructor(url, { subprotocol = 'dioramap-v1' } = {}) {
    super();
    this.url = url;
    this.subprotocol = subprotocol;
    this.ws = null;
    this.sessionToken = null;
    // A text frame that's expecting one binary follow-up. `kind` tells us
    // which WS-client event to emit when the binary arrives:
    //   'artifact' → full artifact, from ARTIFACT_HEADER
    //   'chunk'    → streaming chunk, from STREAM_CHUNK (B9 dual-delivery,
    //                PLAN-VIEWER-PORT §7)
    // At most one of these is outstanding at any time: the server guarantees
    // text-before-binary ordering per artifact.
    this.pendingBinary = null;
    // Wall-clock ms of the last frame received (any kind). Lets callers run
    // a "no progress" watchdog without hooking every event type.
    this.lastMessageAt = 0;
    // Set from SESSION_STARTED (processors ≥ 0.3.0); null before that.
    this.processorVersion = null;
  }

  connect() {
    this._emit('state', { state: 'connecting' });
    const ws = new WebSocket(this.url, [this.subprotocol]);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;

    ws.addEventListener('open', () => this._emit('state', { state: 'connected' }));
    // A browser 'error' event carries no detail; the 'close' that always
    // follows it does (code 1006 = abnormal closure / never connected).
    ws.addEventListener('close', (ev) => this._emit('state', {
      state: 'disconnected', code: ev.code, reason: ev.reason, wasClean: ev.wasClean, url: this.url,
    }));
    ws.addEventListener('error', () => this._emit('state', { state: 'error', url: this.url }));
    ws.addEventListener('message', (ev) => this._onMessage(ev));
  }

  send(msg) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      throw new Error(`WS not open (readyState=${this.ws?.readyState})`);
    }
    this.ws.send(JSON.stringify(msg));
  }

  // Convenience wrappers for the four L1-relevant client → server messages.
  catalog(pipeline) {
    this.send(pipeline ? { t: 'CATALOG', pipeline } : { t: 'CATALOG' });
  }

  run(pipeline, input, subscribe, params, testCaseLabel) {
    const msg = { t: 'RUN', pipeline, input, subscribe };
    if (params && Object.keys(params).length > 0) msg.params = params;
    if (testCaseLabel) msg.test_case_label = testCaseLabel;
    this.send(msg);
  }

  subscribe(artifact) {
    this.send({ t: 'SUBSCRIBE', artifact });
  }

  cancel() { this.send({ t: 'CANCEL' }); }

  // Invalidate the intent-map entry for the step producing `artifact`.
  // Next run recomputes that step; upstream stays cached. Requires a prior
  // RUN on this session (server needs the last input to compute identity).
  invalidate(artifact) {
    this.send({ t: 'INVALIDATE', artifact });
  }

  // L6 — list past runs from the processor's run journal. `filters`
  // accepts { limit?, before?, pipeline?, input_hash?, success? }.
  listRuns(filters = {}) {
    this.send({ t: 'LIST_RUNS', ...filters });
  }

  // L6 — replay a past run's produced artifacts from cache. `journalId`
  // is the cross-restart-stable id from a LIST_RUNS_RESPONSE row. Server
  // emits one ARTIFACT_HEADER per produced artifact present in the
  // store (rehydrated=true on the headers) and a final
  // REHYDRATE_COMPLETE / REHYDRATE_PARTIAL summary.
  rehydrateRun(journalId) {
    this.send({ t: 'REHYDRATE_RUN', journal_id: journalId });
  }

  _onMessage(ev) {
    this.lastMessageAt = Date.now();
    // Binary frame: expected to follow a buffered header (ARTIFACT_HEADER
    // for a full artifact or STREAM_CHUNK for one piece of a streaming one).
    if (ev.data instanceof ArrayBuffer) {
      const pending = this.pendingBinary;
      this.pendingBinary = null;
      if (!pending) {
        this._emit('protocol-error', {
          msg: 'binary frame with no preceding header',
          byteLength: ev.data.byteLength,
        });
        return;
      }
      if (pending.kind === 'artifact') {
        this._emit('artifact', { header: pending.header, bytes: ev.data });
      } else if (pending.kind === 'chunk') {
        this._emit('stream-chunk', { header: pending.header, bytes: ev.data });
      }
      return;
    }

    let msg;
    try {
      msg = JSON.parse(ev.data);
    } catch {
      this._emit('protocol-error', { msg: 'bad JSON', raw: ev.data });
      return;
    }

    switch (msg.t) {
      case 'SESSION_STARTED':
      case 'RECONNECTED':
        this.sessionToken = msg.session_token;
        if (msg.processor_version) this.processorVersion = msg.processor_version;
        this._emit('session', msg);
        break;
      case 'CATALOG_RESPONSE':
        this._emit('catalog', msg);
        break;
      case 'RUN_STARTED':
        this._emit('run-started', msg);
        break;
      case 'STEP_STARTED':
        this._emit('step-started', msg);
        break;
      case 'STEP_COMPLETE':
        this._emit('step-complete', msg);
        break;
      case 'ARTIFACT_HEADER':
        this.pendingBinary = { kind: 'artifact', header: msg };
        break;
      case 'STREAM_CHUNK':
        // Dual delivery (PLAN-VIEWER-PORT §7.3): a STREAM_CHUNK sequence
        // ends with a final=true chunk AND a trailing ARTIFACT_HEADER for
        // the concatenated bytes. Both go through this same pending slot,
        // one after the other — ARTIFACT_HEADER arrives only after the
        // final chunk's binary has been consumed.
        this.pendingBinary = { kind: 'chunk', header: msg };
        break;
      case 'RUN_COMPLETE':
        this._emit('run-complete', msg);
        break;
      case 'RUN_FAILED':
        // Terminal frame for a run that did not complete (docs/ERRORS.md):
        // { run_id, code, msg, step?, step_token?, detail?, pipeline?,
        //   input?, processor_version, duration_us, late_subscribe? }.
        // Processors < 0.3.0 never send it — they send only ERROR.
        this._emit('run-failed', msg);
        break;
      case 'SUBSCRIBE_ACK':
        this._emit('subscribe-ack', msg);
        break;
      case 'INVALIDATED':
        this._emit('invalidated', msg);
        break;
      case 'LIST_RUNS_RESPONSE':
        this._emit('list-runs-response', msg);
        break;
      case 'REHYDRATE_COMPLETE':
      case 'REHYDRATE_PARTIAL':
        this._emit('rehydrate-done', msg);
        break;
      case 'ERROR':
        this._emit('error-msg', msg);
        break;
      default:
        this._emit('unknown', msg);
    }
  }

  _emit(type, detail) {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }
}
