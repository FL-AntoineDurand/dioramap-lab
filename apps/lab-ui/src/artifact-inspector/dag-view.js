// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// DAG view — Blender-style node editor, ported to v3 from
// lidar-algos-bench-v2/ui/public/artifact-graph-view.js (the legacy lab UI).
//
// L3 shape:
//   - one node per step; each node has a header, a collapsible params block,
//     a left column of input ports (one per read), and a right column of
//     write tiles (one per write). Pipeline-input enters as a synthetic root
//     node.
//   - edges are horizontal cubic béziers from an output tile's right port to
//     the consuming step's left input port (artifact-name-matched).
//   - per-artifact load state (idle/loading/loaded/cached) renders as a
//     colored dot on the output tile.
//   - pan (drag) + zoom (wheel, centered on cursor).

import dagre from 'dagre';

const SVG_NS = 'http://www.w3.org/2000/svg';
const NODE_PAD = 10;
const PORT_ROW_H = 22;    // row height per input port / output tile
const TILE_WIDTH = 170;
const READ_COL_WIDTH = 130; // visual width of a read-port label column
const NODE_MIN_WIDTH = READ_COL_WIDTH + TILE_WIDTH + NODE_PAD * 3; // pad left / gap / pad right
const HEADER_HEIGHT = 28;
const PARAMS_ROW_HEIGHT = 22;
const PORT_DOT_RADIUS = 4;

// artifact-kind → surface icon (5 surfaces per §5.3 of PLAN-LAB-UI-PORT.md).
const KIND_ICON = {
  0: '▦', 1: '▦', 2: '▦', 3: '▦', 4: '▦', 5: '▦', 6: '▦',
  7: '{}', 8: '{}', 9: '{}',
  10: '▦', 11: '▦', 12: '▦',
};

export class DagView extends EventTarget {
  constructor(container, { overrides } = {}) {
    super();
    this.container = container;
    this.overrides = overrides;
    this.state = {
      pipeline: null,
      steps: [],
      artifacts: [],
      artifactState: new Map(),  // artifact name → 'idle'|'loading'|'loaded'|'cached'
      paramsOpen: new Set(),
      zoom: 1,
      panX: 0,
      panY: 0,
    };
    // Layout cache so setArtifactState can update a tile in place without a
    // full re-render. Keyed by artifact name; value is the tile <g> element.
    this._tileByArtifact = new Map();
    this._build();
    // Re-render when any external override changes (e.g. run panel's
    // reset-all button) so accent styling + values stay in sync.
    this.overrides?.addEventListener('override-changed', () => {
      if (this.state.pipeline) this._render();
    });
  }

  _build() {
    this.container.classList.add('dag-host');
    this.container.innerHTML = `
      <div class="dag-empty">Select a pipeline to inspect its DAG.</div>
      <svg class="dag-svg" xmlns="http://www.w3.org/2000/svg">
        <defs>
          <marker id="dag-arrow" viewBox="0 0 8 8" refX="7" refY="4"
                  markerWidth="5" markerHeight="5" orient="auto-start-reverse">
            <path d="M0,1 L7,4 L0,7 z" fill="currentColor"/>
          </marker>
        </defs>
        <g class="dag-world"></g>
      </svg>
      <div class="dag-legend mono">
        <span><i class="dag-legend-dot" data-state="idle"></i>idle</span>
        <span><i class="dag-legend-dot" data-state="loading"></i>loading</span>
        <span><i class="dag-legend-dot" data-state="loaded"></i>fresh</span>
        <span><i class="dag-legend-dot" data-state="cached"></i>cached</span>
      </div>
    `;
    this.$empty = this.container.querySelector('.dag-empty');
    this.$svg = this.container.querySelector('.dag-svg');
    this.$world = this.container.querySelector('.dag-world');
    this._installPanZoom();
  }

  _installPanZoom() {
    let dragging = false, lastX = 0, lastY = 0;
    this.$svg.addEventListener('pointerdown', (ev) => {
      if (ev.target.closest('.dag-tile, .dag-node-header')) return;
      dragging = true; lastX = ev.clientX; lastY = ev.clientY;
      this.$svg.setPointerCapture(ev.pointerId);
    });
    this.$svg.addEventListener('pointermove', (ev) => {
      if (!dragging) return;
      this.state.panX += ev.clientX - lastX;
      this.state.panY += ev.clientY - lastY;
      lastX = ev.clientX; lastY = ev.clientY;
      this._applyTransform();
    });
    this.$svg.addEventListener('pointerup', (ev) => {
      dragging = false;
      this.$svg.releasePointerCapture(ev.pointerId);
    });
    this.$svg.addEventListener('wheel', (ev) => {
      ev.preventDefault();
      const delta = ev.deltaY > 0 ? 0.9 : 1.1;
      const newZoom = Math.min(2.5, Math.max(0.3, this.state.zoom * delta));
      const rect = this.$svg.getBoundingClientRect();
      const cx = ev.clientX - rect.left, cy = ev.clientY - rect.top;
      this.state.panX = cx - (cx - this.state.panX) * (newZoom / this.state.zoom);
      this.state.panY = cy - (cy - this.state.panY) * (newZoom / this.state.zoom);
      this.state.zoom = newZoom;
      this._applyTransform();
    }, { passive: false });
  }

  _applyTransform() {
    this.$world.setAttribute(
      'transform',
      `translate(${this.state.panX},${this.state.panY}) scale(${this.state.zoom})`,
    );
  }

  setPipeline(pipeline, { steps, artifacts }) {
    this.state.pipeline = pipeline;
    this.state.steps = steps ?? [];
    this.state.artifacts = artifacts ?? [];
    this.state.artifactState.clear();
    for (const a of this.state.artifacts) this.state.artifactState.set(a.name, 'idle');
    this._render();
  }

  setArtifactState(artifactName, state) {
    this.state.artifactState.set(artifactName, state);
    const tile = this._tileByArtifact.get(artifactName);
    if (tile) tile.dataset.state = state;
  }

  /**
   * Mark a step node's run state. States:
   *   'running'  — step is currently executing (yellow ring, like a
   *                 loading-tier artifact tile).
   *   'cached'   — step completed in this run, served from cache.
   *   'fresh'    — step ran fresh in this run.
   *   'failed'   — step failed (RUN_FAILED / ERROR, docs/ERRORS.md).
   *   undefined  — clear; fall back to neutral.
   * Persisted on the SVG <g class="dag-node" data-step-name="..."> via
   * a `data-state` attribute that the stylesheet keys off.
   */
  setStepState(stepName, state) {
    if (!this._stepGroupByName) this._stepGroupByName = new Map();
    let g = this._stepGroupByName.get(stepName);
    if (!g) {
      // Lazily index. _renderStepNode runs once per render; cache groups
      // here so the per-step lookup stays O(1).
      g = this.$world?.querySelector?.(`.dag-node[data-step-name="${stepName}"]`);
      if (g) this._stepGroupByName.set(stepName, g);
    }
    if (!g) return;
    if (state) g.dataset.state = state;
    else delete g.dataset.state;
  }

  clearAllStepStates() {
    if (!this._stepGroupByName) return;
    for (const g of this._stepGroupByName.values()) delete g.dataset.state;
  }

  // Set the "currently focused" output tile. Adds a `focused` class to the
  // matching tile and removes it from every other. Mirrors the overlay's
  // layer-list focus state so DAG + layer list stay in sync.
  setFocused(artifactName) {
    this.state.focusedArtifact = artifactName;
    for (const [name, tile] of this._tileByArtifact) {
      tile.classList.toggle('focused', name === artifactName);
    }
  }

  clearArtifactStates() {
    for (const name of this.state.artifactState.keys()) {
      this.state.artifactState.set(name, 'idle');
    }
    for (const tile of this._tileByArtifact.values()) tile.dataset.state = 'idle';
  }

  _render() {
    // setStepState's cache is keyed by the previous render's <g> nodes;
    // re-rendering replaces them, so drop the cache.
    this._stepGroupByName?.clear?.();
    this.$world.innerHTML = '';
    this._tileByArtifact.clear();
    if (!this.state.pipeline || this.state.steps.length === 0) {
      this.$empty.hidden = false; this.$svg.hidden = true;
      return;
    }
    this.$empty.hidden = true; this.$svg.hidden = false;

    // Index artifact → producing step; detect pipeline-input usage.
    const producerByArtifact = new Map();
    const inputConsumers = new Set();
    for (const s of this.state.steps) {
      for (const w of s.writes ?? []) producerByArtifact.set(w.name, s.name);
      for (const r of s.reads ?? []) if (r === 'pipeline-input') inputConsumers.add(s.name);
    }
    const hasInput = inputConsumers.size > 0;

    // Build dagre graph and compute node sizes. `multigraph: true` lets us
    // key edges by artifact name when two steps share more than one
    // read/write pairing (e.g. laz-extract-disc emits four artifacts and
    // downstream consumers may read more than one).
    const g = new dagre.graphlib.Graph({ directed: true, multigraph: true });
    g.setGraph({ rankdir: 'LR', nodesep: 30, ranksep: 90, marginx: 20, marginy: 20 });
    g.setDefaultEdgeLabel(() => ({}));

    for (const s of this.state.steps) {
      const nReads = (s.reads ?? []).length;
      const nWrites = (s.writes ?? []).length;
      const paramsOpen = this.state.paramsOpen.has(s.name) && s.params != null;
      const paramsLines = s.params == null
        ? 0
        : (paramsOpen ? Math.max(1, Object.keys(s.params).length) : 1);
      const portsRows = Math.max(nReads, nWrites);
      const height =
        HEADER_HEIGHT +
        paramsLines * PARAMS_ROW_HEIGHT +
        portsRows * PORT_ROW_H +
        NODE_PAD;
      g.setNode(s.name, {
        step: s,
        width: NODE_MIN_WIDTH,
        height,
        paramsOpen, paramsLines,
      });
    }
    if (hasInput) {
      g.setNode('__input', {
        isInput: true,
        width: 140, height: PORT_ROW_H * 2,
      });
    }

    // Edges, one per (step_read → source_step's_write) pairing. The artifact
    // name flows as edge data so we can match output/input ports.
    for (const s of this.state.steps) {
      for (const r of s.reads ?? []) {
        const from = r === 'pipeline-input' ? '__input' : producerByArtifact.get(r);
        if (!from || !g.hasNode(from)) continue;
        // Dagre coalesces multiple edges between the same nodes into one for
        // layout purposes; our edge rendering goes below it and uses port
        // positions, so we need unique edge keys per artifact.
        g.setEdge(from, s.name, { artifact: r }, r);
      }
    }

    dagre.layout(g);

    // Compute port positions per node. Read ports live on the left edge at
    // (nodeLeft, nodeTop + HEADER + params + readIdx*ROW + ROW/2). Write
    // tiles live on the right column at the mirror y. Store these on the
    // graph node so edges + renderers share them.
    for (const id of g.nodes()) {
      const node = g.node(id);
      if (node.isInput) {
        node.outPortY = new Map([['pipeline-input', node.y]]);
        continue;
      }
      const top = node.y - node.height / 2;
      const left = node.x - node.width / 2;
      const portsTop =
        top + HEADER_HEIGHT + node.paramsLines * PARAMS_ROW_HEIGHT + 2;
      const inPortY = new Map();
      const outPortY = new Map();
      (node.step.reads ?? []).forEach((r, i) => {
        inPortY.set(r, portsTop + i * PORT_ROW_H + PORT_ROW_H / 2);
      });
      (node.step.writes ?? []).forEach((w, i) => {
        outPortY.set(w.name, portsTop + i * PORT_ROW_H + PORT_ROW_H / 2);
      });
      node.left = left;
      node.right = left + node.width;
      node.inPortY = inPortY;
      node.outPortY = outPortY;
    }

    // Render edges as horizontal bezier under the nodes.
    for (const e of g.edges()) {
      const src = g.node(e.v);
      const dst = g.node(e.w);
      const artifact = e.name; // edge key
      if (!artifact) continue;
      const srcX = src.isInput ? src.x + src.width / 2 : src.right;
      const srcY = src.isInput
        ? src.y
        : (src.outPortY?.get(artifact) ?? src.y);
      const tgtX = dst.left;
      const tgtY = dst.inPortY?.get(artifact) ?? dst.y;
      const dx = Math.max(40, Math.abs(tgtX - srcX) * 0.4);
      const d = `M ${srcX.toFixed(1)} ${srcY.toFixed(1)} ` +
                `C ${(srcX + dx).toFixed(1)} ${srcY.toFixed(1)}, ` +
                  `${(tgtX - dx).toFixed(1)} ${tgtY.toFixed(1)}, ` +
                  `${tgtX.toFixed(1)} ${tgtY.toFixed(1)}`;
      const path = document.createElementNS(SVG_NS, 'path');
      path.setAttribute('class', 'dag-edge');
      path.setAttribute('d', d);
      path.setAttribute('marker-end', 'url(#dag-arrow)');
      const title = document.createElementNS(SVG_NS, 'title');
      title.textContent = artifact;
      path.appendChild(title);
      this.$world.appendChild(path);
    }

    // Render nodes.
    for (const id of g.nodes()) {
      const node = g.node(id);
      if (node.isInput) {
        this.$world.appendChild(renderInputNode(node.x - node.width / 2,
                                                node.y - node.height / 2, node));
        continue;
      }
      this.$world.appendChild(this._renderStepNode(node));
    }

    // Size the SVG viewBox.
    const graph = g.graph();
    this.$svg.setAttribute('viewBox', `0 0 ${graph.width} ${graph.height}`);
    this.state.panX = 0; this.state.panY = 0; this.state.zoom = 1;
    this._applyTransform();
  }

  _renderStepNode(node) {
    const { left, step, width, height, paramsOpen, paramsLines, inPortY, outPortY } = node;
    const top = node.y - height / 2;

    const g = document.createElementNS(SVG_NS, 'g');
    g.setAttribute('class', 'dag-node');
    g.setAttribute('transform', `translate(${left},${top})`);
    g.dataset.stepName = step.name;

    const rect = document.createElementNS(SVG_NS, 'rect');
    rect.setAttribute('class', 'dag-node-bg');
    rect.setAttribute('width', String(width));
    rect.setAttribute('height', String(height));
    rect.setAttribute('rx', '5');
    g.appendChild(rect);

    // Header (click → toggle params).
    const header = document.createElementNS(SVG_NS, 'g');
    header.setAttribute('class', 'dag-node-header');
    const headerRect = document.createElementNS(SVG_NS, 'rect');
    headerRect.setAttribute('width', String(width));
    headerRect.setAttribute('height', String(HEADER_HEIGHT));
    headerRect.setAttribute('rx', '5');
    header.appendChild(headerRect);
    const title = document.createElementNS(SVG_NS, 'text');
    title.setAttribute('class', 'dag-node-title');
    title.setAttribute('x', String(NODE_PAD));
    title.setAttribute('y', String(HEADER_HEIGHT / 2 + 4));
    title.textContent = `${step.name}  v${step.version}`;
    header.appendChild(title);
    if (step.params != null) {
      const chev = document.createElementNS(SVG_NS, 'text');
      chev.setAttribute('class', 'dag-node-chev');
      chev.setAttribute('x', String(width - NODE_PAD));
      chev.setAttribute('y', String(HEADER_HEIGHT / 2 + 4));
      chev.setAttribute('text-anchor', 'end');
      chev.textContent = paramsOpen ? '▾' : '▸';
      header.appendChild(chev);
      header.addEventListener('click', () => {
        if (this.state.paramsOpen.has(step.name)) this.state.paramsOpen.delete(step.name);
        else this.state.paramsOpen.add(step.name);
        this._render();
      });
    }
    g.appendChild(header);

    let cursorY = HEADER_HEIGHT;

    // Params (collapsible).
    if (step.params != null) {
      if (paramsOpen) {
        // Render each param as an editable row via SVG foreignObject so we
        // get native HTML inputs (number / text / checkbox) with their
        // keyboard behavior intact. Inputs save overrides to the overrides
        // store; the "reset" icon clears back to the pipeline default.
        for (const [k, baseVal] of Object.entries(step.params)) {
          const fo = document.createElementNS(SVG_NS, 'foreignObject');
          fo.setAttribute('x', String(NODE_PAD + 4));
          fo.setAttribute('y', String(cursorY));
          fo.setAttribute('width', String(node.width - NODE_PAD * 2 - 8));
          fo.setAttribute('height', String(PARAMS_ROW_HEIGHT));
          fo.appendChild(this._buildParamRow(step.name, k, baseVal));
          g.appendChild(fo);
          cursorY += PARAMS_ROW_HEIGHT;
        }
      } else {
        const hint = document.createElementNS(SVG_NS, 'text');
        hint.setAttribute('class', 'dag-node-param-hint');
        hint.setAttribute('x', String(NODE_PAD + 8));
        hint.setAttribute('y', String(cursorY + PARAMS_ROW_HEIGHT - 4));
        // Mark overridden keys with a leading ● in the collapsed summary.
        const ov = this.overrides?.forStep(this.state.pipeline, step.name);
        const keys = Object.keys(step.params).map((k) =>
          ov && Object.prototype.hasOwnProperty.call(ov, k) ? `●${k}` : k
        );
        hint.textContent = `params: ${keys.join(', ')}`;
        g.appendChild(hint);
        cursorY += PARAMS_ROW_HEIGHT;
      }
    }

    // Read input ports on left, write output tiles on right.
    const reads = step.reads ?? [];
    const writes = step.writes ?? [];
    const rows = Math.max(reads.length, writes.length);
    for (let i = 0; i < rows; i++) {
      const rowY = cursorY + i * PORT_ROW_H;
      if (i < reads.length) {
        g.appendChild(this._renderInputPort(reads[i], 0, rowY));
      }
      if (i < writes.length) {
        const tile = this._renderOutputTile(
          writes[i],
          width - NODE_PAD - TILE_WIDTH, rowY, TILE_WIDTH,
        );
        g.appendChild(tile);
      }
    }

    return g;
  }

  _renderInputPort(readName, x, y) {
    const g = document.createElementNS(SVG_NS, 'g');
    g.setAttribute('class', 'dag-port-in');
    g.setAttribute('transform', `translate(${x},${y})`);

    const dot = document.createElementNS(SVG_NS, 'circle');
    dot.setAttribute('cx', '0');
    dot.setAttribute('cy', String(PORT_ROW_H / 2));
    dot.setAttribute('r', String(PORT_DOT_RADIUS));
    dot.setAttribute('class', 'dag-port-in-dot');
    g.appendChild(dot);

    const label = document.createElementNS(SVG_NS, 'text');
    label.setAttribute('class', 'dag-port-in-label');
    label.setAttribute('x', String(PORT_DOT_RADIUS + 6));
    label.setAttribute('y', String(PORT_ROW_H / 2 + 4));
    label.textContent = readName;
    g.appendChild(label);

    const title = document.createElementNS(SVG_NS, 'title');
    title.textContent = `reads: ${readName}`;
    g.appendChild(title);
    return g;
  }

  _renderOutputTile(write, x, y, width) {
    const tile = document.createElementNS(SVG_NS, 'g');
    tile.setAttribute('class', 'dag-tile');
    tile.setAttribute('transform', `translate(${x},${y})`);
    tile.dataset.name = write.name;
    tile.dataset.visibility = write.visibility;
    tile.dataset.kind = String(write.kind);
    tile.dataset.state = this.state.artifactState.get(write.name) ?? 'idle';

    const rect = document.createElementNS(SVG_NS, 'rect');
    rect.setAttribute('width', String(width));
    rect.setAttribute('height', String(PORT_ROW_H - 4));
    rect.setAttribute('y', '2');
    rect.setAttribute('rx', '3');
    tile.appendChild(rect);

    const icon = document.createElementNS(SVG_NS, 'text');
    icon.setAttribute('class', 'dag-tile-icon');
    icon.setAttribute('x', '8');
    icon.setAttribute('y', String(PORT_ROW_H / 2 + 4));
    icon.textContent = KIND_ICON[write.kind] ?? '?';
    tile.appendChild(icon);

    const label = document.createElementNS(SVG_NS, 'text');
    label.setAttribute('class', 'dag-tile-label');
    label.setAttribute('x', '26');
    label.setAttribute('y', String(PORT_ROW_H / 2 + 4));
    label.textContent = write.name;
    tile.appendChild(label);

    // Load-state dot on the right; doubles as the output port anchor.
    const dot = document.createElementNS(SVG_NS, 'circle');
    dot.setAttribute('cx', String(width - 1));
    dot.setAttribute('cy', String(PORT_ROW_H / 2));
    dot.setAttribute('r', String(PORT_DOT_RADIUS));
    dot.setAttribute('class', 'dag-tile-dot');
    tile.appendChild(dot);

    // Tooltip: full artifact name + load-state meaning + visibility.
    const title = document.createElementNS(SVG_NS, 'title');
    title.textContent = '';
    tile.appendChild(title);
    const updateTooltip = () => {
      title.textContent =
        `${write.name}  (${write.visibility})\n` +
        `state: ${tile.dataset.state}  — ` +
        STATE_DESCR[tile.dataset.state];
    };
    updateTooltip();
    const mo = new MutationObserver(updateTooltip);
    mo.observe(tile, { attributes: true, attributeFilter: ['data-state'] });

    tile.addEventListener('click', (ev) => {
      ev.stopPropagation();
      this.dispatchEvent(new CustomEvent('artifact-clicked', {
        detail: { name: write.name, kind: write.kind },
      }));
    });
    tile.addEventListener('contextmenu', (ev) => {
      ev.preventDefault();
      ev.stopPropagation();
      this._showTileContextMenu(ev.clientX, ev.clientY, write.name);
    });

    this._tileByArtifact.set(write.name, tile);
    if (this.state.focusedArtifact === write.name) tile.classList.add('focused');
    return tile;
  }

  _showTileContextMenu(x, y, artifactName) {
    this._closeContextMenu();
    const menu = document.createElement('div');
    menu.className = 'dag-context-menu';
    menu.style.left = `${x}px`;
    menu.style.top  = `${y}px`;
    menu.innerHTML = `
      <button type="button" data-action="invalidate">
        <span class="dag-ctx-icon">🗑</span>
        <span>Invalidate cache</span>
      </button>
      <button type="button" data-action="subscribe">
        <span class="dag-ctx-icon">▦</span>
        <span>Load (SUBSCRIBE)</span>
      </button>
    `;
    document.body.appendChild(menu);
    this._openMenu = menu;

    menu.addEventListener('click', (ev) => {
      const btn = ev.target.closest('button[data-action]');
      if (!btn) return;
      const action = btn.dataset.action;
      this._closeContextMenu();
      if (action === 'invalidate') {
        this.dispatchEvent(new CustomEvent('artifact-invalidate-requested', {
          detail: { name: artifactName },
        }));
      } else if (action === 'subscribe') {
        this.dispatchEvent(new CustomEvent('artifact-clicked', {
          detail: { name: artifactName },
        }));
      }
    });

    const outside = (ev) => { if (!menu.contains(ev.target)) this._closeContextMenu(); };
    const key = (ev) => { if (ev.key === 'Escape') this._closeContextMenu(); };
    // Defer so the current click that opened the menu doesn't immediately
    // dismiss it.
    setTimeout(() => {
      document.addEventListener('click', outside);
      document.addEventListener('contextmenu', outside);
      document.addEventListener('keydown', key);
    }, 0);
    this._menuDismiss = () => {
      document.removeEventListener('click', outside);
      document.removeEventListener('contextmenu', outside);
      document.removeEventListener('keydown', key);
    };
  }

  _closeContextMenu() {
    if (this._openMenu) { this._openMenu.remove(); this._openMenu = null; }
    if (this._menuDismiss) { this._menuDismiss(); this._menuDismiss = null; }
  }

  // Build an HTML row for one param key. Uses an input whose type is
  // inferred from the base value's JS type — numbers become number inputs,
  // booleans become checkboxes, everything else is a text input that
  // JSON-parses on submit. If the user has an override set, its value is
  // shown; the row is marked overridden (accent color + reset button).
  _buildParamRow(stepName, paramName, baseVal) {
    const root = document.createElement('div');
    root.className = 'dag-param-row';
    const pipeline = this.state.pipeline;
    const hasOverride = this.overrides?.has(pipeline, stepName, paramName) ?? false;
    if (hasOverride) root.classList.add('overridden');
    const currentVal = hasOverride
      ? this.overrides.get(pipeline, stepName, paramName)
      : baseVal;

    const label = document.createElement('span');
    label.className = 'dag-param-key';
    label.textContent = `${paramName}:`;
    root.appendChild(label);

    const input = this._buildInputForType(baseVal, currentVal);
    input.className = 'dag-param-input';
    input.addEventListener('change', () => {
      const parsed = this._readInput(input, baseVal);
      if (parsed === FAIL) return;
      if (deepEqual(parsed, baseVal)) {
        this.overrides?.clear(pipeline, stepName, paramName);
      } else {
        this.overrides?.set(pipeline, stepName, paramName, parsed);
      }
    });
    // Stop the pointerdown from bubbling into the SVG pan handler.
    ['pointerdown', 'click', 'mousedown'].forEach((ev) =>
      input.addEventListener(ev, (e) => e.stopPropagation()));
    root.appendChild(input);

    const reset = document.createElement('button');
    reset.type = 'button';
    reset.className = 'dag-param-reset';
    reset.title = 'reset to pipeline default';
    reset.textContent = '↺';
    reset.hidden = !hasOverride;
    reset.addEventListener('click', (ev) => {
      ev.stopPropagation();
      this.overrides?.clear(pipeline, stepName, paramName);
    });
    root.appendChild(reset);

    return root;
  }

  _buildInputForType(baseVal, currentVal) {
    if (typeof baseVal === 'boolean') {
      const el = document.createElement('input');
      el.type = 'checkbox';
      el.checked = !!currentVal;
      return el;
    }
    if (typeof baseVal === 'number') {
      const el = document.createElement('input');
      el.type = 'number';
      el.step = 'any';
      el.value = String(currentVal);
      return el;
    }
    if (typeof baseVal === 'string') {
      const el = document.createElement('input');
      el.type = 'text';
      el.value = currentVal;
      return el;
    }
    // Arrays / objects / null → JSON text input.
    const el = document.createElement('input');
    el.type = 'text';
    el.value = JSON.stringify(currentVal);
    return el;
  }

  _readInput(el, baseVal) {
    if (el.type === 'checkbox') return el.checked;
    if (el.type === 'number') {
      const n = Number(el.value);
      if (!Number.isFinite(n)) return FAIL;
      return n;
    }
    if (typeof baseVal === 'string') return el.value;
    try { return JSON.parse(el.value); } catch { return FAIL; }
  }
}

const FAIL = Symbol('param-parse-fail');

function deepEqual(a, b) {
  if (a === b) return true;
  if (typeof a !== typeof b) return false;
  if (a && b && typeof a === 'object') {
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) if (!deepEqual(a[k], b[k])) return false;
    return true;
  }
  return false;
}

const STATE_DESCR = {
  idle: 'not yet produced in this session',
  loading: 'SUBSCRIBE in flight',
  loaded: 'computed + delivered this run',
  cached: 'served from store (content-hash hit)',
};

function renderInputNode(x, y, node) {
  const g = document.createElementNS(SVG_NS, 'g');
  g.setAttribute('class', 'dag-input-node');
  g.setAttribute('transform', `translate(${x},${y})`);
  const rect = document.createElementNS(SVG_NS, 'rect');
  rect.setAttribute('width', String(node.width));
  rect.setAttribute('height', String(node.height));
  rect.setAttribute('rx', '4');
  g.appendChild(rect);
  const title = document.createElementNS(SVG_NS, 'text');
  title.setAttribute('class', 'dag-input-label');
  title.setAttribute('x', String(node.width / 2));
  title.setAttribute('y', String(node.height / 2 + 4));
  title.setAttribute('text-anchor', 'middle');
  title.textContent = 'pipeline-input';
  g.appendChild(title);
  return g;
}

function jsonCompact(v) {
  if (v == null) return String(v);
  if (typeof v === 'string') return `"${v}"`;
  if (typeof v === 'number' || typeof v === 'boolean') return String(v);
  try { return JSON.stringify(v); } catch { return String(v); }
}
