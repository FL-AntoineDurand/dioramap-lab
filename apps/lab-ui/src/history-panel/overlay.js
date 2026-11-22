// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// L6.3 — full-page history overlay.
//
// Hierarchically above everything: the user is zooming OUT to inspect
// past runs, not narrowing the current view. Mounts as a position:fixed
// overlay covering the whole viewport, z-index above the gear popover
// and the loading bar.
//
// Open via:
//   - `#open-history` button in the topbar
//   - keyboard shortcut `H` (no input focused)
//
// Close via:
//   - Esc (any time the overlay is open)
//   - backdrop click
//   - explicit ✕ in the header
//
// Click a row → REHYDRATE_RUN with that row's journal_id, close overlay,
// switch center pane to inspect mode. Existing ARTIFACT_HEADER handlers
// in app.js replay the artifacts into the inspector behind the (now
// closed) overlay — no special client-side decode path needed.

export class HistoryOverlay extends EventTarget {
  /**
   * @param {Object} cfg
   * @param {import('@dioramap/protocol').WSClient} cfg.client
   */
  constructor({ client }) {
    super();
    this.client = client;
    this.runs = [];
    this.filters = {
      pipeline: '',
      input_substring: '',
      success: 'any',     // 'any' | 'success' | 'fail'
      cache: 'any',       // 'any' | 'fresh' | 'cached'
    };
    this._build();
    this._wireWS();
    this._open = false;
  }

  _build() {
    const root = document.createElement('div');
    root.className = 'history-overlay';
    root.style.display = 'none';
    root.innerHTML = `
      <div class="history-backdrop"></div>
      <div class="history-card">
        <header class="history-head">
          <span class="history-title">Run history</span>
          <span class="history-count" id="history-count"></span>
          <span class="history-spacer"></span>
          <button class="history-refresh" type="button" title="Refresh">↻</button>
          <button class="history-close" type="button" title="Close (Esc)">✕</button>
        </header>
        <div class="history-filters">
          <select class="history-pipeline">
            <option value="">all pipelines</option>
          </select>
          <input class="history-search" type="search" placeholder="filter input substring…" />
          <select class="history-success">
            <option value="any">any</option>
            <option value="success">success only</option>
            <option value="fail">failed only</option>
          </select>
          <select class="history-cache">
            <option value="any">any cache state</option>
            <option value="fresh">fresh runs only</option>
            <option value="cached">cached runs only</option>
          </select>
        </div>
        <div class="history-list" id="history-list"></div>
        <footer class="history-foot">
          <span class="history-foot-hint">click a row to rehydrate · Esc closes · H toggles</span>
        </footer>
      </div>
    `;
    document.body.appendChild(root);
    this.root = root;
    this.list = root.querySelector('#history-list');
    this.countEl = root.querySelector('#history-count');
    this.pipelineSel = root.querySelector('.history-pipeline');
    this.searchEl = root.querySelector('.history-search');
    this.successSel = root.querySelector('.history-success');
    this.cacheSel = root.querySelector('.history-cache');

    root.querySelector('.history-close').addEventListener('click', () => this.close());
    root.querySelector('.history-refresh').addEventListener('click', () => this.refresh());
    root.querySelector('.history-backdrop').addEventListener('click', () => this.close());
    this.pipelineSel.addEventListener('change', () => {
      this.filters.pipeline = this.pipelineSel.value;
      this.refresh();
    });
    this.searchEl.addEventListener('input', () => {
      this.filters.input_substring = this.searchEl.value;
      this._render();   // client-side filter; no need to re-query
    });
    this.successSel.addEventListener('change', () => {
      this.filters.success = this.successSel.value;
      this.refresh();
    });
    this.cacheSel.addEventListener('change', () => {
      this.filters.cache = this.cacheSel.value;
      this._render();   // client-side filter
    });
  }

  _wireWS() {
    this.client.addEventListener('list-runs-response', (ev) => {
      this.runs = ev.detail.runs ?? [];
      this._populatePipelineDropdown();
      this._render();
    });
    this.client.addEventListener('rehydrate-done', (ev) => {
      const m = ev.detail;
      const tag = m.t === 'REHYDRATE_PARTIAL' ? 'PARTIAL' : 'OK';
      const missing = m.missing ? ` missing=${JSON.stringify(m.missing)}` : '';
      console.log(`[history] rehydrate ${tag} jid=${m.journal_id} emitted=${m.n_emitted}${missing}`);
    });
  }

  _populatePipelineDropdown() {
    const seen = new Set();
    for (const r of this.runs) seen.add(r.pipeline);
    const current = this.pipelineSel.value;
    // Reset; rebuild from `all pipelines` + each unique value.
    this.pipelineSel.innerHTML = '<option value="">all pipelines</option>';
    for (const name of Array.from(seen).sort()) {
      const opt = document.createElement('option');
      opt.value = name;
      opt.textContent = name;
      this.pipelineSel.appendChild(opt);
    }
    if (current && seen.has(current)) this.pipelineSel.value = current;
  }

  _render() {
    const filtered = this._applyClientFilters(this.runs);
    this.countEl.textContent = `${filtered.length} run${filtered.length === 1 ? '' : 's'}`;
    this.list.innerHTML = '';
    if (filtered.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'history-empty';
      empty.textContent = this.runs.length === 0
        ? 'No runs in journal yet — run a pipeline first.'
        : 'No runs match the current filters.';
      this.list.appendChild(empty);
      return;
    }
    for (const r of filtered) this.list.appendChild(this._renderRow(r));
  }

  _applyClientFilters(rows) {
    const sub = (this.filters.input_substring || '').toLowerCase();
    return rows.filter((r) => {
      if (this.filters.cache === 'fresh' && r.n_cache_hits > 0 && r.n_executed === 0) return false;
      if (this.filters.cache === 'cached' && r.n_executed > 0) return false;
      if (sub) {
        const blob = `${r.test_case_label || ''} ${r.input_hash || ''} ${(r.subscribe_set || []).join(' ')}`;
        if (!blob.toLowerCase().includes(sub)) return false;
      }
      return true;
    });
  }

  _renderRow(r) {
    const row = document.createElement('div');
    row.className = 'history-row';
    row.dataset.journalId = String(r.journal_id);
    const time = new Date(Math.floor(r.started_at / 1000)).toLocaleString();
    const dur = r.duration_us > 1_000_000
      ? `${(r.duration_us / 1_000_000).toFixed(1)}s`
      : `${(r.duration_us / 1000).toFixed(0)}ms`;
    const status = r.success
      ? (r.n_executed === 0 ? 'cached' : 'fresh')
      : 'fail';
    const label = r.test_case_label || `<span class="history-row-input">input ${r.input_hash.slice(0, 10)}…</span>`;
    row.innerHTML = `
      <span class="history-row-time">${time}</span>
      <span class="history-row-pipeline">${r.pipeline}</span>
      <span class="history-row-label">${label}</span>
      <span class="history-row-status" data-status="${status}">${status}</span>
      <span class="history-row-counts">${r.n_executed}/${r.n_executed + r.n_cache_hits}</span>
      <span class="history-row-dur">${dur}</span>
    `;
    row.addEventListener('click', () => this._onRowClick(r));
    return row;
  }

  _onRowClick(r) {
    this.dispatchEvent(new CustomEvent('rehydrate-requested', { detail: r }));
    this.client.rehydrateRun(r.journal_id);
    this.close();
  }

  refresh() {
    if (!this.client) return;
    const filters = { limit: 200 };
    if (this.filters.pipeline) filters.pipeline = this.filters.pipeline;
    if (this.filters.success === 'success') filters.success = true;
    if (this.filters.success === 'fail') filters.success = false;
    try { this.client.listRuns(filters); } catch (e) { console.warn('[history] listRuns', e); }
  }

  open() {
    if (this._open) return;
    this._open = true;
    this.root.style.display = 'flex';
    this.refresh();
  }

  close() {
    if (!this._open) return;
    this._open = false;
    this.root.style.display = 'none';
  }

  toggle() { this._open ? this.close() : this.open(); }
  isOpen() { return this._open; }
}
