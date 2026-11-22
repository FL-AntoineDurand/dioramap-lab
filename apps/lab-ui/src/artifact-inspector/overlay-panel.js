// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Overlay panel — L3 layout: layer-list column + render stack.
//
// The overlay panel shows one row per loaded artifact in a left-side list
// and a stacked set of render blocks on the right. 3D-surface artifacts
// appear in the layer list but their actual visible form lives in the 3D
// viewer, not in the render stack. Clicking a layer-list row scrolls the
// right column to that row's render block (if any).
//
// Per §5.2 + §5.3 of PLAN-LAB-UI-PORT.md.

import { highlightJson } from './json-highlight.js';

// Type icons: five surfaces (§5.3). Matches the DAG view's tile vocabulary.
// The surface is determined per-render-call (first surface used per artifact
// wins when picking the row icon) so mixed renders (e.g. 3D + Chart) show
// the "richest" icon — currently 3D > Chart > SVG > JSON > HTML.
const SURFACE_ICON = {
  '3d':   '▦',
  svg:   '</>',
  chart: '📊',
  json:  '{}',
  html:  '‹›',
};
const SURFACE_PRIORITY = { '3d': 5, chart: 4, svg: 3, json: 2, html: 1 };

export class OverlayPanel extends EventTarget {
  // listHost + stackHost may be the same element (legacy side-by-side
  // layout) or two different elements (L3.4 three-column inspect mode).
  // The panel renders its list-items into listHost and its render blocks
  // into stackHost; the two can be placed anywhere in the DOM.
  constructor({ listHost, stackHost }) {
    super();
    this.listHost = listHost;
    this.stackHost = stackHost;
    this.entries = new Map(); // layerId → { row, block, body, surfaces }
    this.focusedLayerId = null;
    this._build();
    this._onRemove = null;
  }

  _build() {
    this.listHost.classList.add('overlay-list');
    this.listHost.innerHTML = `
      <div class="pane-header"><span>Artifacts</span></div>
      <ul class="overlay-list-items"></ul>
    `;
    this.$list = this.listHost.querySelector('.overlay-list-items');

    this.stackHost.classList.add('overlay-stack');
    // The stack-host wrapper now contains a sibling .pane-header (set
    // in index.html) plus a scrollable #stack-body inner div. Mount our
    // render blocks inside #stack-body if it exists; fall back to the
    // host itself for tests / non-lab callers that don't pre-render
    // the inner structure.
    this.$stack = this.stackHost.querySelector('#stack-body') ?? this.stackHost;
    this.$stack.innerHTML = '';
  }

  setRemoveHandler(fn) { this._onRemove = fn; }

  // Ensure layer-list row + render-stack block exist for `layerId`. Returns
  // { row, block }; row is the layer-list <li>, block is the stack <section>.
  _ensureEntry(layerId, label) {
    const existing = this.entries.get(layerId);
    if (existing) return existing;

    const row = document.createElement('li');
    row.className = 'overlay-layer-row';
    row.dataset.layerId = layerId;
    row.dataset.surface = 'pending';
    row.innerHTML = `
      <span class="overlay-layer-icon">·</span>
      <span class="overlay-layer-name">${escape(label ?? layerId)}</span>
      <button type="button" class="overlay-layer-remove" title="remove">×</button>
    `;
    this.$list.appendChild(row);

    const block = document.createElement('section');
    block.className = 'overlay-block-group';
    block.dataset.layerId = layerId;
    block.innerHTML = `
      <header class="overlay-block-header">
        <span class="overlay-block-title">${escape(label ?? layerId)}</span>
      </header>
      <div class="overlay-block-body"></div>
    `;
    this.$stack.appendChild(block);

    const entry = {
      row,
      block,
      body: block.querySelector('.overlay-block-body'),
      surfaces: new Set(),
      label: label ?? layerId,
    };
    this.entries.set(layerId, entry);

    row.addEventListener('click', (ev) => {
      if (ev.target.classList.contains('overlay-layer-remove')) return;
      this.focus(layerId);
      // Layer-list clicks should also update the DAG view's focus; the
      // orchestrator (app.js) bridges the two via this event so this
      // module stays decoupled from the DAG view.
      const artifactName = artifactFromLayerId(layerId);
      this.dispatchEvent(new CustomEvent('focus-requested', {
        detail: { layerId, artifactName },
      }));
    });
    row.querySelector('.overlay-layer-remove').addEventListener('click', () => {
      if (this._onRemove) this._onRemove(layerId);
    });

    return entry;
  }

  // Public focus API. Highlights the layer-list row AND its render-stack
  // block, scrolls the block into view (for non-3D surfaces), and remembers
  // the selection so both stay highlighted until another layer is chosen.
  focus(layerId) {
    const entry = this.entries.get(layerId);
    if (!entry) return;
    this.focusedLayerId = layerId;
    for (const row of this.$list.querySelectorAll('.overlay-layer-row')) {
      row.classList.toggle('focused', row.dataset.layerId === layerId);
    }
    for (const block of this.$stack.querySelectorAll('.overlay-block-group')) {
      block.classList.toggle('focused', block.dataset.layerId === layerId);
    }
    if (entry.surfaces.has('3d') && entry.surfaces.size === 1) {
      // Pure-3D artifact: no stack block to scroll to. The highlighted row
      // is the visual cue; the 3D scene itself is the payload.
      return;
    }
    entry.block.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  }

  // Called by RenderContext. `type` is one of 'svg'|'chart'|'json'|'html'|'3d'.
  // 3d is tracked for the layer-list icon only; no stack block is appended.
  appendRender(layerId, type, payload) {
    const entry = this._ensureEntry(layerId, this._pendingLabel(layerId));
    entry.surfaces.add(type);
    this._refreshSurfaceIcon(entry);

    if (type === '3d') return; // no stack block for 3D

    const div = document.createElement('div');
    div.className = `overlay-block overlay-block-${type}`;
    switch (type) {
      case 'svg':
        div.appendChild(buildSvg(payload));
        break;
      case 'chart':
        payload.build(div);
        break;
      case 'json':
        // highlightJson escapes internally — do not double-escape.
        div.innerHTML = `<pre class="json">${highlightJson(stringify(payload.value))}</pre>`;
        break;
      case 'html':
        if (payload.content instanceof Node) div.appendChild(payload.content);
        else div.innerHTML = payload.content;
        break;
    }
    entry.body.appendChild(div);
  }

  setLabel(layerId, label) {
    const entry = this._ensureEntry(layerId, label);
    entry.label = label;
    entry.row.querySelector('.overlay-layer-name').textContent = label;
    entry.block.querySelector('.overlay-block-title').textContent = label;
  }

  _pendingLabel(layerId) {
    const e = this.entries.get(layerId);
    return e?.label ?? layerId;
  }

  _refreshSurfaceIcon(entry) {
    const top = Array.from(entry.surfaces)
      .sort((a, b) => (SURFACE_PRIORITY[b] ?? 0) - (SURFACE_PRIORITY[a] ?? 0))[0];
    entry.row.dataset.surface = top ?? 'pending';
    entry.row.querySelector('.overlay-layer-icon').textContent = SURFACE_ICON[top] ?? '·';
    // If the artifact has ONLY a 3D surface, mark the stack block compact so
    // the stack doesn't show an empty body.
    entry.block.classList.toggle('three-d-only',
      entry.surfaces.size === 1 && entry.surfaces.has('3d'));
  }


  removeEntry(layerId) {
    const entry = this.entries.get(layerId);
    if (!entry) return;
    entry.row.remove();
    entry.block.remove();
    this.entries.delete(layerId);
  }

  clearAll() {
    for (const id of Array.from(this.entries.keys())) this.removeEntry(id);
    this.focusedLayerId = null;
  }
}

// Layer ids are built as `${pipeline}::${artifactName}` by the inspector.
// Split on the first `::` so artifact names containing literal "::" round-trip.
function artifactFromLayerId(layerId) {
  const idx = layerId.indexOf('::');
  return idx < 0 ? layerId : layerId.slice(idx + 2);
}

function buildSvg({ name, gElement, width, height }) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('class', 'overlay-svg');
  svg.setAttribute('viewBox', `0 0 ${width ?? 100} ${height ?? 100}`);
  svg.setAttribute('preserveAspectRatio', 'xMidYMid meet');
  if (gElement) svg.appendChild(gElement);
  if (name) svg.dataset.svgName = name;
  return svg;
}

function stringify(v) {
  if (v === undefined || v === null) return String(v);
  try { return JSON.stringify(v, replacer, 2); }
  catch { return String(v); }
}

function replacer(_k, v) {
  if (ArrayBuffer.isView(v)) {
    return {
      __typedArray: v.constructor.name,
      length: v.length,
      head: Array.from(v.slice(0, 8)),
      tail: v.length > 8 ? '…' : undefined,
    };
  }
  return v;
}

function escape(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}
