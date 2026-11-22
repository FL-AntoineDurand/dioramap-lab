// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Legend overlay — corner panel inside the 3D viewer that stacks
// per-visible-layer legends.
//
// Each visualizer that wants a legend calls ctx.renderLegend(spec) on
// its RenderContext. The overlay listens to sceneState's
// `visibility-changed` events and only displays legends for layers
// currently visible in the scene (so toggling a layer off auto-hides
// its legend; toggling on auto-shows it).
//
// Legend `spec` shape:
//   {
//     title: string,                                // layer/artifact name
//     swatches?: [{ label: string, color: string, count?: number }, ...],
//     gradient?: { min: number, max: number, colorStart: string,
//                  colorEnd: string, scale?: 'linear'|'log',
//                  format?: (v) => string },
//     note?: string,                                 // optional caption
//   }
//
// One overlay per ArtifactInspector — ctor takes the sceneState so it
// can filter on visibility, and the host element to attach the panel
// inside (typically the viewer pane element).

const STYLE_CACHE = new WeakSet();

function ensureStyles(doc) {
  if (STYLE_CACHE.has(doc)) return;
  STYLE_CACHE.add(doc);
  const css = `
    .legend-overlay {
      position: absolute; top: 12px; left: 12px; z-index: 50;
      display: flex; flex-direction: column; gap: 8px;
      max-width: 240px; max-height: calc(100% - 24px);
      overflow-y: auto;
      pointer-events: none;
      font: 11px monospace;
    }
    .legend-overlay > * { pointer-events: auto; }
    .legend-card {
      background: rgba(13,17,23,0.92);
      border: 1px solid #30363d;
      border-radius: 4px;
      padding: 6px 8px;
      color: #c9d1d9;
      box-shadow: 0 2px 6px rgba(0,0,0,0.4);
      min-width: 220px;
    }
    .legend-card .legend-title {
      color: #8b949e; font-weight: 600; margin-bottom: 4px;
      white-space: nowrap; text-overflow: ellipsis; overflow: hidden;
    }
    .legend-card .legend-swatches {
      display: flex; flex-direction: column; gap: 2px;
    }
    .legend-card .legend-row {
      display: flex; align-items: center; gap: 6px; line-height: 14px;
    }
    .legend-card .legend-swatch {
      display: inline-block; width: 10px; height: 10px;
      border: 1px solid rgba(0,0,0,0.5); flex-shrink: 0;
    }
    .legend-card .legend-label {
      flex: 1 1 auto; white-space: nowrap; text-overflow: ellipsis; overflow: hidden;
    }
    .legend-card .legend-count {
      color: #6e7681; flex-shrink: 0;
    }
    .legend-card .legend-gradient-bar {
      width: 100%; height: 8px; border-radius: 1px;
      margin: 2px 0;
    }
    .legend-card .legend-gradient-labels {
      display: flex; justify-content: space-between; color: #8b949e;
      font-size: 10px;
    }
    .legend-card .legend-note {
      color: #6e7681; font-size: 10px; margin-top: 4px;
    }
  `;
  const style = doc.createElement('style');
  style.textContent = css;
  doc.head.appendChild(style);
}

export class LegendOverlay {
  // host: an element to attach the panel inside (its position must be
  // relative or absolute). sceneState: optional — used to filter by
  // visibility. When absent, all legends always show.
  constructor(host, { sceneState } = {}) {
    if (host) ensureStyles(host.ownerDocument || document);
    this.host = host;
    this.sceneState = sceneState ?? null;
    this._legends = new Map(); // layerId → spec
    this._layerIdsByContext = new Map(); // ctxLayerId → Set<layerId>
    this._panel = document.createElement('div');
    this._panel.className = 'legend-overlay';
    if (host) host.appendChild(this._panel);

    if (this.sceneState) {
      this._onVisChanged = () => this._render();
      this.sceneState.addEventListener('visibility-registered', this._onVisChanged);
      this.sceneState.addEventListener('visibility-changed',    this._onVisChanged);
      this.sceneState.addEventListener('subtree-changed',       this._onVisChanged);
      this.sceneState.addEventListener('subtree-cleared',       this._onVisChanged);
    }
  }

  // Register a legend keyed by a 3D layer id (the leaf id used in
  // ctx.render3D's layer.id). When that layer is toggled off in the
  // visibility tree, the legend is hidden automatically.
  registerLegend(layerId, spec, ctxLayerId) {
    this._legends.set(layerId, spec);
    if (ctxLayerId) {
      let s = this._layerIdsByContext.get(ctxLayerId);
      if (!s) { s = new Set(); this._layerIdsByContext.set(ctxLayerId, s); }
      s.add(layerId);
    }
    this._render();
  }

  unregisterLegend(layerId) {
    this._legends.delete(layerId);
    this._render();
  }

  // Drop every legend a given RenderContext registered. Called on
  // ctx.destroy().
  unregisterByContext(ctxLayerId) {
    const set = this._layerIdsByContext.get(ctxLayerId);
    if (!set) return;
    for (const lid of set) this._legends.delete(lid);
    this._layerIdsByContext.delete(ctxLayerId);
    this._render();
  }

  clearAll() {
    this._legends.clear();
    this._layerIdsByContext.clear();
    this._render();
  }

  _isLayerVisible(layerId) {
    if (!this.sceneState) return true;
    try {
      // VisibilityTree.node(idOrPath) returns a snapshot or null. For
      // not-yet-attached layers we treat as visible (legend appears
      // before the 3D layer is even built — common during streaming
      // re-renders).
      const node = this.sceneState.visibility.node?.(layerId);
      if (!node) return true;
      // Tri-state: 'all', 'some', 'none' for groups; bool for leaves.
      const v = node.visible ?? node.visibleDerived;
      if (v === false || v === 'none') return false;
      return true;
    } catch (_) {
      return true;
    }
  }

  _render() {
    if (!this._panel) return;
    this._panel.innerHTML = '';
    // Group-key dedup: a renderer that owns N visually-identical layers
    // (e.g. one Points layer per plane, all sharing one colour-bar
    // legend) can register the same spec N times — once per layer — but
    // with a shared `groupKey`. Whichever of those N layers is visible
    // wins; only ONE card per groupKey is drawn. Specs without groupKey
    // render every time their layer is visible (pre-existing behaviour).
    const seenGroups = new Set();
    for (const [layerId, spec] of this._legends) {
      if (!this._isLayerVisible(layerId)) continue;
      if (spec.groupKey) {
        if (seenGroups.has(spec.groupKey)) continue;
        seenGroups.add(spec.groupKey);
      }
      this._panel.appendChild(this._renderCard(spec));
    }
  }

  _renderCard(spec) {
    const card = document.createElement('div');
    card.className = 'legend-card';
    if (spec.title) {
      const t = document.createElement('div');
      t.className = 'legend-title';
      t.textContent = spec.title;
      t.title = spec.title;
      card.appendChild(t);
    }
    if (spec.swatches?.length) {
      const list = document.createElement('div');
      list.className = 'legend-swatches';
      for (const sw of spec.swatches) {
        const row = document.createElement('div');
        row.className = 'legend-row';
        const dot = document.createElement('span');
        dot.className = 'legend-swatch';
        dot.style.background = sw.color;
        const label = document.createElement('span');
        label.className = 'legend-label';
        label.textContent = sw.label;
        label.title = sw.label;
        row.append(dot, label);
        if (sw.count != null) {
          const cnt = document.createElement('span');
          cnt.className = 'legend-count';
          cnt.textContent = String(sw.count);
          row.appendChild(cnt);
        }
        list.appendChild(row);
      }
      card.appendChild(list);
    }
    if (spec.gradient) {
      const g = spec.gradient;
      const fmt = g.format || ((v) => Number(v).toFixed(3));
      const bar = document.createElement('div');
      bar.className = 'legend-gradient-bar';
      // `stops` (array of CSS colour strings) wins over the
      // colorStart→colorEnd pair so multi-stop ramps (e.g. HSL hue
      // sweeps that pass through cyan/green/yellow) render correctly
      // — a 2-colour CSS gradient interpolates linearly in RGB and
      // misses the intermediate hues the actual cloud renders.
      const stops = Array.isArray(g.stops) && g.stops.length >= 2
        ? g.stops
        : [g.colorStart, g.colorEnd];
      bar.style.background = `linear-gradient(to right, ${stops.join(', ')})`;
      card.appendChild(bar);
      const lbls = document.createElement('div');
      lbls.className = 'legend-gradient-labels';
      const left  = document.createElement('span'); left.textContent  = fmt(g.min);
      const mid   = document.createElement('span'); mid.textContent   = fmt((g.min + g.max) / 2);
      const right = document.createElement('span'); right.textContent = fmt(g.max);
      lbls.append(left, mid, right);
      card.appendChild(lbls);
    }
    if (spec.note) {
      const note = document.createElement('div');
      note.className = 'legend-note';
      note.textContent = spec.note;
      card.appendChild(note);
    }
    return card;
  }
}
