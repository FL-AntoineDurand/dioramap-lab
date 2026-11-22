// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Step timeline — L4 Gantt slice.
//
// Each step gets a horizontal bar positioned on a shared time axis. The
// axis auto-scales to max(end) across visible bars, re-rendered on every
// STEP_STARTED / STEP_COMPLETE. Parallel steps simply land at different
// rows at similar X positions, which is what we want — the visual cue for
// concurrency is bars aligning vertically.
//
// The server doesn't send wall-clock start times; we stamp them client-side
// from `performance.now()` on STEP_STARTED. STEP_COMPLETE carries an
// authoritative `duration_us`, which we use for the bar width rather than
// (now - start) so the bar matches what the runner reports.

const PREFIX_COL_WIDTH = 160; // px — reserved for the step name

export class StepTimeline {
  constructor(container) {
    this.container = container;
    this.entries = new Map(); // step name → entry state
    this._t0 = null;          // performance.now() at the first step_started
    this._extraRows = [];     // non-step summary rows (subscribe marker, run complete, error)
    this._build();
  }

  _build() {
    this.container.classList.add('timeline-host');
    this.container.innerHTML = `
      <div class="gantt-track-header">
        <span class="gantt-track-label mono">step</span>
        <div class="gantt-axis"></div>
      </div>
      <div class="gantt-rows"></div>
      <ul class="timeline-list"></ul>
    `;
    this.$axis = this.container.querySelector('.gantt-axis');
    this.$rows = this.container.querySelector('.gantt-rows');
    this.$list = this.container.querySelector('.timeline-list');
  }

  // Clear all state + reserve rows for a known step plan. Called at RUN.
  reset(stepPlan) {
    this.entries.clear();
    this._extraRows.length = 0;
    this._t0 = null;
    this.$rows.innerHTML = '';
    this.$list.innerHTML = '';
    this.$axis.innerHTML = '';
    for (const step of stepPlan ?? []) this._ensureRow(step, { preflight: true });
  }

  noteLateSubscribe(artifact) {
    this._appendSummary(`subscribe: ${artifact}`, 'tail-walking DAG…');
  }

  onStepStarted(step) {
    const now = performance.now();
    if (this._t0 == null) this._t0 = now;
    const entry = this._ensureRow(step);
    entry.startRel = now - this._t0;
    entry.status = 'running';
    entry.row.dataset.state = 'running';
    this._renderBar(entry);
    this._reflowAxis();
  }

  onStepComplete({ step, cache_hit, duration_us }) {
    const entry = this._ensureRow(step);
    if (entry.startRel == null) {
      // Step completed without a prior STEP_STARTED (cached steps can
      // fire this way during late-subscribe). Anchor it at t0 so the bar
      // still renders.
      if (this._t0 == null) this._t0 = performance.now();
      entry.startRel = performance.now() - this._t0;
    }
    entry.durationMs = duration_us / 1000;
    entry.cacheHit = !!cache_hit;
    entry.status = 'done';
    entry.row.dataset.state = 'done';
    this._renderBar(entry);
    this._reflowAxis();
  }

  onRunComplete(summary) {
    const dur = (summary.duration_us / 1000).toFixed(1);
    this._appendSummary(
      'run complete',
      `${dur} ms · executed ${summary.steps_executed} · hits ${summary.cache_hits}`,
    );
  }

  onSubscribeAck({ artifact, duration_us, steps_executed, cache_hits }) {
    const dur = (duration_us / 1000).toFixed(1);
    this._appendSummary(
      'subscribe complete',
      `${dur} ms · executed ${steps_executed} · hits ${cache_hits}`,
    );
  }

  onError(msg) {
    const li = document.createElement('li');
    li.dataset.state = 'error';
    li.innerHTML =
      `<span class="timeline-step-name">ERROR</span>
       <span class="timeline-step-status mono">${escape(msg.step ?? '')} · ${escape(msg.msg ?? msg.code ?? 'unknown')}</span>`;
    this.$list.appendChild(li);
  }

  _ensureRow(step, { preflight = false } = {}) {
    const existing = this.entries.get(step);
    if (existing) return existing;

    const row = document.createElement('div');
    row.className = 'gantt-row';
    row.dataset.state = preflight ? 'pending' : 'running';
    row.innerHTML = `
      <span class="gantt-step-name mono">${escape(step)}</span>
      <div class="gantt-track">
        <div class="gantt-bar" hidden></div>
        <span class="gantt-duration mono"></span>
      </div>
    `;
    this.$rows.appendChild(row);
    const entry = {
      step, row,
      bar: row.querySelector('.gantt-bar'),
      durationLabel: row.querySelector('.gantt-duration'),
      startRel: null,
      durationMs: null,
      status: preflight ? 'pending' : 'running',
      cacheHit: false,
    };
    this.entries.set(step, entry);
    return entry;
  }

  _renderBar(entry) {
    const total = this._totalMs();
    if (total <= 0) return;
    const startFrac = Math.max(0, entry.startRel / total);
    // For still-running steps we don't yet know the duration; draw a
    // minimal-width bar so the viewer can at least see which row is live.
    const durMs = entry.durationMs ?? Math.max(1, total * 0.01);
    const widthFrac = Math.min(1 - startFrac, durMs / total);
    entry.bar.hidden = false;
    entry.bar.style.left = `${(startFrac * 100).toFixed(2)}%`;
    entry.bar.style.width = `${(widthFrac * 100).toFixed(2)}%`;
    entry.bar.classList.toggle('cached', entry.cacheHit);
    if (entry.durationMs != null) {
      const ms = entry.durationMs.toFixed(1);
      entry.durationLabel.innerHTML = entry.cacheHit
        ? `<span class="badge cached">CACHED</span> ${ms} ms`
        : `${ms} ms`;
    } else {
      entry.durationLabel.textContent = 'running…';
    }
  }

  // Reflow all bars when the axis scale changes (new max end).
  _reflowAxis() {
    const total = this._totalMs();
    for (const entry of this.entries.values()) {
      if (entry.startRel != null) this._renderBar(entry);
    }
    // Axis ticks: at most 5 labels across the width.
    this.$axis.innerHTML = '';
    if (total <= 0) return;
    const nTicks = 5;
    for (let i = 0; i <= nTicks; i++) {
      const frac = i / nTicks;
      const ms = total * frac;
      const tick = document.createElement('span');
      tick.className = 'gantt-axis-tick mono';
      tick.style.left = `${(frac * 100).toFixed(2)}%`;
      tick.textContent = ms >= 1000 ? `${(ms / 1000).toFixed(1)} s` : `${Math.round(ms)} ms`;
      this.$axis.appendChild(tick);
    }
  }

  _totalMs() {
    let max = 0;
    for (const e of this.entries.values()) {
      if (e.startRel == null) continue;
      const end = e.startRel + (e.durationMs ?? 0);
      if (end > max) max = end;
    }
    // Minimum 1ms so the axis doesn't collapse before the first complete.
    return max || 1;
  }

  _appendSummary(name, status) {
    const li = document.createElement('li');
    li.dataset.state = 'summary';
    li.innerHTML =
      `<span class="timeline-step-name">${escape(name)}</span>
       <span class="timeline-step-status mono">${escape(status)}</span>`;
    this.$list.appendChild(li);
    this._extraRows.push(li);
  }
}

function escape(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}
