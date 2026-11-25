// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// errors.js — one error shape for every way a run can fail (docs/ERRORS.md).
//
// Sources, all normalised into a DioramapError:
//   - RUN_FAILED frame (processor ≥ 0.3.0): code, msg, step, detail, input…
//   - ERROR frame: request-level rejections ({code, msg}) and, from older
//     processors, the only signal of a step failure ({run_id, step, msg})
//   - the WebSocket closing mid-run, or never opening
//   - a client-side watchdog when nothing arrives for too long
//
// `toReport()` is the "technical details" block: plain JSON a user can copy
// into a GitHub issue. `issueUrl()` pre-fills that issue.

export const DEFAULT_ISSUE_REPO = 'FL-AntoineDurand/dioramap-lab';

export class DioramapError extends Error {
  /**
   * @param {Object} f
   * @param {string} f.code      stable code: NO_DATA, UPSTREAM_HTTP, …, CONNECTION_LOST
   * @param {string} f.message   one-line technical message
   * @param {string} [f.source]  'run-failed' | 'server-error' | 'connection' | 'watchdog'
   * @param {string} [f.step]    friendly step name
   * @param {Object} [f.detail]  structured evidence (url, http_status, body_excerpt, …)
   * @param {string} [f.runId]
   * @param {string} [f.pipeline]
   * @param {Object} [f.input]   RUN input (lat/lon/radius)
   * @param {string} [f.processorVersion]
   * @param {Object} [f.raw]     the frame or event this was built from
   */
  constructor(f) {
    super(f.message || f.code || 'unknown error');
    this.name = 'DioramapError';
    this.code = f.code || 'INTERNAL';
    this.source = f.source || 'unknown';
    this.step = f.step ?? null;
    this.detail = f.detail ?? null;
    this.runId = f.runId ?? null;
    this.pipeline = f.pipeline ?? null;
    this.input = f.input ?? null;
    this.processorVersion = f.processorVersion ?? null;
    this.clientVersion = f.clientVersion ?? null;
    this.backendUrl = f.backendUrl ?? null;
    this.time = new Date().toISOString();
    this.raw = f.raw ?? null;
  }

  /** Plain object for "copy technical details" / issue bodies. */
  toReport() {
    const r = {
      code: this.code,
      message: this.message,
      step: this.step,
      run_id: this.runId,
      pipeline: this.pipeline,
      input: this.input,
      detail: this.detail,
      processor_version: this.processorVersion,
      client_version: this.clientVersion,
      backend: stripKey(this.backendUrl),
      source: this.source,
      time: this.time,
    };
    if (typeof navigator !== 'undefined' && navigator.userAgent) r.user_agent = navigator.userAgent;
    if (typeof location !== 'undefined' && location.origin) r.page = location.origin + location.pathname;
    for (const k of Object.keys(r)) if (r[k] == null) delete r[k];
    return r;
  }

  toJSON() { return this.toReport(); }
}

// Never leak an API key or access token into a pasted report.
function stripKey(url) {
  if (!url) return url;
  return String(url).replace(/([?&](?:key|token)=)[^&]*/gi, '$1<redacted>');
}

export function errorFromRunFailed(msg, ctx = {}) {
  return new DioramapError({
    code: msg.code,
    message: msg.msg,
    source: 'run-failed',
    step: msg.step,
    detail: msg.detail,
    runId: msg.run_id,
    pipeline: msg.pipeline ?? ctx.pipeline,
    input: msg.input ?? ctx.input,
    processorVersion: msg.processor_version ?? ctx.processorVersion,
    clientVersion: ctx.clientVersion,
    backendUrl: ctx.backendUrl,
    raw: msg,
  });
}

// ERROR frames. `msg.msg` is the field the processor sends (older SDKs
// read `message`, which never existed — the reason embedders only ever
// saw "server error").
export function errorFromServerError(msg, ctx = {}) {
  let code = msg.code;
  let text = msg.msg ?? msg.message ?? '';
  // "[CODE] message" — step failures from processors ≥ 0.3.0.
  const m = /^\[([A-Z_]+)\]\s*(.*)$/.exec(text);
  if (!code && m) { code = m[1]; text = m[2]; }
  if (!code && text === 'cancelled') code = 'CANCELLED';
  if (!code) code = msg.step ? 'STEP_FAILED' : 'SERVER_ERROR';
  if (!text && msg.artifact) text = `artifact ${msg.artifact}`;
  return new DioramapError({
    code,
    message: text || code,
    source: 'server-error',
    step: msg.step,
    runId: msg.run_id,
    pipeline: ctx.pipeline,
    input: ctx.input,
    processorVersion: ctx.processorVersion,
    clientVersion: ctx.clientVersion,
    backendUrl: ctx.backendUrl,
    detail: msg.artifact ? { artifact: msg.artifact } : null,
    raw: msg,
  });
}

/** @param {{code?: number, reason?: string, wasClean?: boolean}} ev  WSClient 'state' detail */
export function errorFromDisconnect(ev, ctx = {}) {
  const neverOpened = !ctx.everConnected;
  const code = neverOpened ? 'CONNECTION_FAILED' : 'CONNECTION_LOST';
  const what = neverOpened ? 'could not connect to the processor' : 'connection to the processor closed mid-run';
  const reason = ev?.reason ? `: ${ev.reason}` : '';
  return new DioramapError({
    code,
    message: `${what} (WebSocket close code ${ev?.code ?? '?'}${reason})`,
    source: 'connection',
    detail: { ws_close_code: ev?.code ?? null, ws_close_reason: ev?.reason || null, was_clean: ev?.wasClean ?? null },
    runId: ctx.runId,
    pipeline: ctx.pipeline,
    input: ctx.input,
    processorVersion: ctx.processorVersion,
    clientVersion: ctx.clientVersion,
    backendUrl: ctx.backendUrl,
    raw: ev,
  });
}

export function errorFromWatchdog(idleMs, ctx = {}) {
  return new DioramapError({
    code: 'STALLED',
    message: `no message from the processor for ${Math.round(idleMs / 1000)} s while a run was active` +
             (ctx.lastStep ? ` (last step started: ${ctx.lastStep})` : ''),
    source: 'watchdog',
    step: ctx.lastStep,
    detail: { idle_ms: idleMs, steps_completed: ctx.stepsCompleted ?? null, steps_total: ctx.stepsTotal ?? null },
    runId: ctx.runId,
    pipeline: ctx.pipeline,
    input: ctx.input,
    processorVersion: ctx.processorVersion,
    clientVersion: ctx.clientVersion,
    backendUrl: ctx.backendUrl,
  });
}

/** Markdown body for a GitHub issue / clipboard. */
export function formatReport(err) {
  const r = err instanceof DioramapError ? err.toReport() : { message: String(err?.message ?? err) };
  return [
    `**${r.code ?? 'ERROR'}**${r.step ? ` in \`${r.step}\`` : ''}: ${r.message}`,
    '',
    '```json',
    JSON.stringify(r, null, 2),
    '```',
  ].join('\n');
}

/**
 * GitHub "new issue" URL with the report pre-filled. Uses the repo's
 * run-failure issue form (field id `report`) when present; GitHub falls
 * back to a blank issue with `body` otherwise. Bodies are truncated to
 * keep the URL under browser limits.
 */
export function issueUrl(err, { repo = DEFAULT_ISSUE_REPO } = {}) {
  const title = `Run failed: ${err.code ?? 'ERROR'}${err.step ? ` in ${err.step}` : ''}`;
  let report = formatReport(err);
  const MAX = 6000;
  if (report.length > MAX) report = report.slice(0, MAX) + '\n… (truncated: use "Copy" for the full report)\n```';
  const q = new URLSearchParams({
    template: 'run-failure.yml',
    title,
    report,
    labels: 'run-failure',
  });
  return `https://github.com/${repo}/issues/new?${q.toString()}`;
}
