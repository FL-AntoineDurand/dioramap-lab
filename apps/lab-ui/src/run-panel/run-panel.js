// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Run panel — L2 slice.
//
// Owns: input JSON textarea, subscribe-artifact checklist, Run + Cancel
// buttons. Emits `run-requested` with `{pipeline, input, subscribe}` when
// the user clicks Run, for the app-level orchestrator to hand off to the
// WS client. A params-editor (§7.8 of the plan) lands in L5.
//
// Per-pipeline input suggestions are hardcoded here for now (pipelines that
// read lat/lon/radius get a Paris default). A more principled solution is
// a pipeline-manifest `example_input` field; defer until the catalog gains
// one (Q2 in the plan).

import { loadInputForPipeline, saveInputForPipeline } from '../layout/preferences.js';

const EXAMPLES = {
  'viewer-pointcloud':               { lat: 48.8566, lon: 2.3522, radius_m: 30 },
  'viewer-pointcloud-with-normals':  { lat: 48.8566, lon: 2.3522, radius_m: 30 },
  'viewer-pointcloud-with-rasters':  { lat: 48.8566, lon: 2.3522, radius_m: 30 },
  'viewer-fetchers':                 { lat: 48.8566, lon: 2.3522, radius_m: 30 },
  'viewer-ortho-rasters':            { lat: 48.8566, lon: 2.3522, radius_m: 30 },
  'demo-chain':                      { base: 5 },
  'array-demo':                      { base: 3 },
  'cube-memoized-demo':              { value: 4 },
  'gpu-smoke':                       {},
};

export class RunPanel extends EventTarget {
  constructor(container, { overrides } = {}) {
    super();
    this.container = container;
    this.overrides = overrides;
    this.pipeline = null;
    this.artifacts = [];
    this._build();
    this.overrides?.addEventListener('override-changed', () => this._refreshOverrideBadge());
  }

  _build() {
    this.container.innerHTML = `
      <div class="run-section">
        <label class="run-label">Pipeline</label>
        <div class="run-pipeline mono"></div>
      </div>
      <div class="run-section">
        <div class="run-input-header">
          <label class="run-label" for="run-input">Input JSON</label>
          <button type="button" id="btn-map-pick" class="run-input-pick" title="pick on map">
            📍 Map
          </button>
        </div>
        <textarea id="run-input" class="mono" spellcheck="false"></textarea>
      </div>
      <div class="run-section">
        <label class="run-label">Subscribe</label>
        <ul class="run-subscribe-list"></ul>
      </div>
      <div class="run-overrides-row" hidden>
        <span class="run-overrides-badge mono">0 overrides</span>
        <button type="button" class="run-overrides-reset">reset all</button>
      </div>
      <div class="run-buttons">
        <button id="btn-run" type="button" class="primary">Run</button>
        <button id="btn-cancel" type="button">Cancel</button>
      </div>
    `;
    this.$pipeline = this.container.querySelector('.run-pipeline');
    this.$input = this.container.querySelector('#run-input');
    this.$subs = this.container.querySelector('.run-subscribe-list');
    this.$run = this.container.querySelector('#btn-run');
    this.$cancel = this.container.querySelector('#btn-cancel');
    this.$mapPick = this.container.querySelector('#btn-map-pick');
    this.$overridesRow = this.container.querySelector('.run-overrides-row');
    this.$overridesBadge = this.container.querySelector('.run-overrides-badge');
    this.$overridesReset = this.container.querySelector('.run-overrides-reset');

    this.$run.addEventListener('click', () => this._fireRun());
    this.$cancel.addEventListener('click', () => this._fireCancel());
    this.$mapPick.addEventListener('click', () => this._firePick());
    this.$overridesReset.addEventListener('click', () => {
      if (this.pipeline) this.overrides?.clearPipeline(this.pipeline);
    });
    // Persist the input JSON whenever the user types or we programmatically
    // update it (via map picker). Debounced so per-keystroke saves don't
    // hammer localStorage.
    this._persistInputDebounced = debounce(() => {
      if (this.pipeline) saveInputForPipeline(this.pipeline, this.$input.value);
    }, 400);
    this.$input.addEventListener('input', this._persistInputDebounced);
  }

  setPipeline(pipeline, artifacts) {
    this.pipeline = pipeline;
    this.artifacts = artifacts;
    this.$pipeline.textContent = pipeline ?? '(none)';
    if (pipeline) {
      // Restore the last-typed input for this pipeline if present,
      // otherwise fall back to the hardcoded example.
      const saved = loadInputForPipeline(pipeline);
      if (saved != null) {
        this.$input.value = saved;
      } else {
        const example = EXAMPLES[pipeline] ?? {};
        this.$input.value = JSON.stringify(example, null, 2);
      }
    } else {
      this.$input.value = '';
    }
    this._renderSubs();
    this._refreshOverrideBadge();
  }

  _refreshOverrideBadge() {
    if (!this.$overridesRow || !this.overrides) return;
    const { count, steps } = this.overrides.summary(this.pipeline);
    if (count === 0) {
      this.$overridesRow.hidden = true;
      return;
    }
    this.$overridesRow.hidden = false;
    this.$overridesBadge.textContent = `${count} override${count === 1 ? '' : 's'} across ${steps} step${steps === 1 ? '' : 's'}`;
  }

  _renderSubs() {
    this.$subs.innerHTML = '';
    if (!this.artifacts?.length) {
      this.$subs.innerHTML = '<li class="empty">no artifacts</li>';
      return;
    }
    // Default: all artifacts subscribed (lab UI is debug-tier — the user
    // wants to see everything by default, including canonical-location and
    // tile manifests that downstream renderers look up as siblings).
    for (const a of this.artifacts) {
      const li = document.createElement('li');
      const id = `sub-${a.name}`;
      const checked = true;
      li.innerHTML = `
        <label>
          <input type="checkbox" id="${id}" data-name="${escape(a.name)}" ${checked ? 'checked' : ''} />
          <span>${escape(a.name)}</span>
          <span class="badge ${a.visibility === 'public' ? 'public' : 'debug'}">${escape(a.visibility)}</span>
        </label>
      `;
      this.$subs.appendChild(li);
    }
  }

  _fireRun() {
    if (!this.pipeline) return;
    let input;
    try {
      input = JSON.parse(this.$input.value || '{}');
    } catch (e) {
      this.dispatchEvent(new CustomEvent('run-error', { detail: `Bad JSON: ${e.message}` }));
      return;
    }
    const boxes = this.$subs.querySelectorAll('input[type=checkbox]:checked');
    const subscribe = Array.from(boxes).map((b) => b.dataset.name);
    if (subscribe.length === 0) {
      this.dispatchEvent(new CustomEvent('run-error', { detail: 'select at least one artifact to subscribe' }));
      return;
    }
    const params = this.overrides?.forRunMessage(this.pipeline) ?? {};
    this.dispatchEvent(new CustomEvent('run-requested', {
      detail: { pipeline: this.pipeline, input, subscribe, params },
    }));
  }

  _fireCancel() {
    this.dispatchEvent(new CustomEvent('cancel-requested'));
  }

  _firePick() {
    // Seed the picker with whatever's currently in the textarea (so
    // re-opening the map re-centers on the current location rather than
    // snapping back to the Paris default).
    let seed = {};
    try { seed = JSON.parse(this.$input.value || '{}'); } catch { /* ignore */ }
    this.dispatchEvent(new CustomEvent('pick-requested', {
      detail: {
        lat: seed.lat,
        lon: seed.lon,
        radius: seed.radius_m ?? seed.radius,
      },
    }));
  }

  // Called by app.js after the picker resolves. Merges the pick into the
  // current input JSON (preserving any other keys), leaving the textarea's
  // formatting clean.
  applyPick({ lat, lon, radius }) {
    let parsed = {};
    try { parsed = JSON.parse(this.$input.value || '{}'); } catch { /* fresh */ }
    parsed.lat = round5(lat);
    parsed.lon = round5(lon);
    parsed.radius_m = radius;
    this.$input.value = JSON.stringify(parsed, null, 2);
    // Programmatic change doesn't fire `input`, so persist explicitly.
    if (this.pipeline) saveInputForPipeline(this.pipeline, this.$input.value);
  }

  setRunning(running) {
    this.$run.disabled = !!running;
    this.$cancel.disabled = !running;
  }
}

function escape(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

function round5(v) { return Math.round(v * 1e5) / 1e5; }

function debounce(fn, wait) {
  let t = null;
  return (...args) => {
    if (t != null) clearTimeout(t);
    t = setTimeout(() => { t = null; fn(...args); }, wait);
  };
}
