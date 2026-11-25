// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Log panel — rolling text console of WS events.
//
// L4 scope: consumes STEP_STARTED / STEP_COMPLETE / ARTIFACT_HEADER /
// RUN_STARTED / RUN_COMPLETE / SUBSCRIBE_ACK / ERROR / SESSION_STARTED
// / RECONNECTED and formats them as `[HH:MM:SS.mmm] [event] details`
// lines. Auto-scrolls to bottom with a pin toggle; Clear button resets.
// A forthcoming LOG_EVENT WS frame type would flow through the same
// `append()` path without changes here.

const MAX_LINES = 1000; // hard cap to protect the DOM if a run spams

export class LogPanel {
  constructor(container, { client }) {
    this.container = container;
    this.client = client;
    this.paused = false;
    this.pinned = true;
    this.lines = 0;
    this._build();
    this._wire();
  }

  _build() {
    this.container.classList.add('log-host');
    // Section title is provided by the column's `.pane-header` upstream
    // (index.html), so the log panel itself only renders the line-count
    // + pin/clear controls + the body.
    this.container.innerHTML = `
      <div class="log-header">
        <span class="log-line-count mono">0</span>
        <span class="log-header-spacer"></span>
        <label class="log-pin">
          <input type="checkbox" checked />
          <span>pin bottom</span>
        </label>
        <button type="button" class="log-clear">clear</button>
      </div>
      <pre class="log-body" tabindex="0"></pre>
    `;
    this.$body = this.container.querySelector('.log-body');
    this.$count = this.container.querySelector('.log-line-count');
    this.$pin = this.container.querySelector('.log-pin input');
    this.$clear = this.container.querySelector('.log-clear');

    this.$pin.addEventListener('change', () => {
      this.pinned = this.$pin.checked;
      if (this.pinned) this._scrollToBottom();
    });
    this.$clear.addEventListener('click', () => this.clear());
  }

  _wire() {
    const on = (type, fmt) => {
      this.client.addEventListener(type, (ev) => this.append(fmt(ev.detail)));
    };
    on('session',       (d) => ({ tag: 'session', text: `SESSION_STARTED tier=${d.tier ?? '?'} token=${short(d.session_token)}` }));
    on('run-started',   (d) => ({ tag: 'run',     text: `RUN_STARTED pipeline=${d.pipeline} plan=[${(d.step_plan ?? []).join(', ')}]` }));
    on('step-started',  (d) => ({ tag: 'step',    text: `STEP_STARTED ${d.step}` }));
    on('step-complete', (d) => ({
      tag: 'step',
      text: `STEP_COMPLETE ${d.step}${d.cache_hit ? ' (cached)' : ''} ${(d.duration_us / 1000).toFixed(1)}ms`,
    }));
    on('artifact', (d) => ({
      tag: 'artifact',
      text: `ARTIFACT ${d.header.artifact} size=${d.header.size}B hash=${short(d.header.hash)}`,
    }));
    on('stream-chunk', (d) => ({
      tag: 'stream',
      text: `STREAM_CHUNK ${d.header.artifact} seq=${d.header.seq} final=${d.header.final} size=${d.header.size}B${d.header.final ? ` hash=${short(d.header.hash)}` : ''}`,
    }));
    on('run-complete',   (d) => ({
      tag: 'run',
      text: `RUN_COMPLETE ${(d.summary.duration_us / 1000).toFixed(1)}ms · executed ${d.summary.steps_executed} · hits ${d.summary.cache_hits}`,
    }));
    on('subscribe-ack',  (d) => ({
      tag: 'sub',
      text: `SUBSCRIBE_ACK ${d.artifact ?? ''} ${(d.duration_us / 1000).toFixed(1)}ms · executed ${d.steps_executed} · hits ${d.cache_hits}`,
    }));
    on('invalidated', (d) => ({
      tag: 'invalidate',
      text: `INVALIDATED ${d.artifact} (step=${d.step}) prev=${short(d.previous_manifest_hash)}`,
    }));
    // RUN_FAILED (docs/ERRORS.md): one line + the full structured detail,
    // so the upstream URL / HTTP status / response excerpt are right here.
    on('run-failed',     (d) => ({
      tag: 'error',
      text: `RUN_FAILED ${d.code} step=${d.step ?? '(run)'} ${d.msg ?? ''}` +
            (d.detail ? `\n    detail ${JSON.stringify(d.detail, null, 2).replace(/\n/g, '\n    ')}` : ''),
    }));
    on('error-msg',      (d) => ({
      tag: 'error',
      text: `ERROR code=${d.code ?? '?'} step=${d.step ?? ''} ${d.msg ?? ''}`,
    }));
  }

  append({ tag, text }) {
    if (this.paused) return;
    const line = document.createElement('span');
    line.className = `log-line log-line-${tag}`;
    line.textContent = `[${tsNow()}] [${tag}] ${text}\n`;
    this.$body.appendChild(line);
    this.lines += 1;
    // Bound the DOM: discard oldest lines past MAX_LINES.
    while (this.lines > MAX_LINES && this.$body.firstChild) {
      this.$body.firstChild.remove();
      this.lines -= 1;
    }
    this.$count.textContent = String(this.lines);
    if (this.pinned) this._scrollToBottom();
  }

  clear() {
    this.$body.innerHTML = '';
    this.lines = 0;
    this.$count.textContent = '0';
  }

  _scrollToBottom() {
    this.$body.scrollTop = this.$body.scrollHeight;
  }
}

function tsNow() {
  const d = new Date();
  const pad = (n, w = 2) => String(n).padStart(w, '0');
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`;
}

function short(s) {
  if (!s) return '';
  return s.length > 12 ? s.slice(0, 12) : s;
}
