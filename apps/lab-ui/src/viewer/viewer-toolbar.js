// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Viewer toolbar — floating gear + popover in the top-right of the 3D scene.
//
// One panel for everything that's "scene state" from the user's perspective:
//   - Projection (perspective / ortho)
//   - Point size slider
//   - Compass toggle
//   - Sky / fog toggles
//   - Sun (date sliders → sceneState.sun.setFromDate)
//   - Layers (visibility tree from sceneState.visibility, with filter +
//     tri-state cascade + hide-all/show-all/defaults)
//
// Mounted inside the viewer's host element so it sits over the canvas and
// moves with the viewport. Pure DOM — no Three.js dependencies.

import { savePref, VIEWER_PREFS } from '../layout/preferences.js';

export class ViewerToolbar {
  constructor(host, { viewer, compass, sceneState, initial = {},
                       inline = false, layersHost = null } = {}) {
    this.host = host;
    this.viewer = viewer;
    this.compass = compass;
    this.sceneState = sceneState ?? null;
    // `inline=true` mounts the toolbar's content directly into `host`
    // without the gear button + popover wrapper — used by the
    // always-visible Scene-Controls column.
    // `layersHost` (inline-only) is a separate DOM element that
    // receives the Layers visibility-tree section, so the column can
    // place a unified `.pane-header` ABOVE that host and a different
    // header above the rest. Without `layersHost`, the Layers section
    // stays in the popover next to Sun.
    this.inline = inline;
    this.layersHost = layersHost;
    this.state = {
      projection: initial.projection ?? 'perspective',
      pointSize: initial.pointSize ?? 0.1,  // world metres (sizeAttenuation:true)
      compass: initial.compass ?? false,
      sky: initial.sky ?? true,
      fog: initial.fog ?? false,
      axes: initial.axes ?? true,
      grid: initial.grid ?? true,
    };
    this._filterText = '';
    this._leafInputs = new Map();   // layerId → leaf checkbox
    // Persisted group expand-state across re-renders. Toggling a leaf
    // fires `subtree-changed` (parent's derived tri-state moves), which
    // calls `_renderTree()` and rebuilds the DOM. Without this set, every
    // group would collapse back to its default (closed) on each leaf
    // click — fix re-applies the user's open/closed choice when the tree
    // is rebuilt. Key = path joined by '/' (group ids).
    this._openPaths = new Set();
    // Tree-rebuild coalescing + active-gating (see _scheduleRenderTree /
    // setActive). `_treeActive` mirrors whether the Layers tree is currently
    // shown (results view) — the app wires it from its center-mode switch so
    // the O(N²) rebuild burst during a pipeline run (tree hidden) is deferred
    // to a single rebuild when the results view appears.
    this._treeActive = true;
    this._treeDirty = false;
    this._treeRenderPending = false;
    this._build();
    // Apply the inherited initial state to the viewer + compass so a
    // reloaded page visibly matches what was saved, not the defaults.
    this.viewer.setProjection(this.state.projection);
    this.viewer.setPointSize(this.state.pointSize);
    this.compass?.setEnabled(this.state.compass);
    this.viewer.setSkyEnabled?.(this.state.sky);
    this.viewer.setFogEnabled?.(this.state.fog);
    this.viewer.setAxesEnabled?.(this.state.axes);
    this.viewer.setGridEnabled?.(this.state.grid);

    if (this.sceneState) {
      // Initial sun values mirror sceneState.sun.state.date. Year is fixed
      // (not user-tunable in lab-ui — same convention as the legacy SDK
      // controls panel; year is irrelevant for sub-day sun-position math).
      this._sunYear = this.sceneState.sun.state.date.year;
      const d = this.sceneState.sun.state.date;
      this.$sunM.value  = d.month;
      this.$sunMVal.textContent = String(d.month);
      this.$sunD.value  = d.day;
      this.$sunDVal.textContent = String(d.day);
      const slot = d.hour * 6 + Math.floor(d.minute / 10);
      this.$sunT.value = slot;
      this.$sunTVal.textContent = formatTime(slot);

      // SceneState event subscriptions — live updates of the layer tree.
      // Coalesce rebuilds: a pipeline run registers hundreds of leaves
      // one-by-one (one event each) and tri-state cascades fire more, so
      // rebuilding the whole tree per event is O(N²). _scheduleRenderTree
      // batches all events in a frame into a single rebuild (and skips it
      // entirely while the tree isn't visible). See _scheduleRenderTree.
      this.sceneState.addEventListener('visibility-registered', () => this._scheduleRenderTree());
      this.sceneState.addEventListener('subtree-changed',       () => this._scheduleRenderTree());
      this.sceneState.addEventListener('subtree-cleared',       () => this._scheduleRenderTree());
      this.sceneState.addEventListener('visibility-changed',    (e) => this._reflectLeaf(e.detail));
      this.sceneState.addEventListener('sun-changed',           (e) => this._reflectSun(e.detail));
      this._renderTree();
    }
  }

  _build() {
    const root = document.createElement('div');
    root.className = this.inline ? 'vw-toolbar vw-toolbar-inline' : 'vw-toolbar';
    // In inline mode skip the gear button + popover wrapper: the panel
    // contents render directly into the column. We still produce the
    // same `.vw-toolbar-popover` div so the rest of `_build()` (which
    // queries it via querySelector) keeps working unchanged.
    const gearBtn = this.inline ? '' : `
      <button type="button" class="vw-toolbar-btn" title="viewer settings" aria-label="viewer settings">
        ${GEAR_SVG}
      </button>`;
    root.innerHTML = `
      ${gearBtn}
      <div class="vw-toolbar-popover" role="menu" ${this.inline ? '' : 'hidden'}>
        <div class="vw-toolbar-row">
          <label class="vw-toolbar-label">Projection</label>
          <div class="vw-pill" role="tablist">
            <button type="button" role="tab" data-value="perspective">perspective</button>
            <button type="button" role="tab" data-value="ortho">ortho</button>
          </div>
        </div>
        <div class="vw-toolbar-row">
          <label class="vw-toolbar-label" for="vw-point-size">Point size</label>
          <input id="vw-point-size" type="range" min="0.02" max="0.5" step="0.02" />
          <span class="vw-toolbar-value mono"></span>
        </div>
        <div class="vw-toolbar-row">
          <label class="vw-toolbar-label">Compass</label>
          <div class="vw-pill" role="tablist">
            <button type="button" role="tab" data-compass="off">off</button>
            <button type="button" role="tab" data-compass="on">on</button>
          </div>
        </div>
        <div class="vw-toolbar-row">
          <label class="vw-toolbar-label">Sky</label>
          <div class="vw-pill" role="tablist">
            <button type="button" role="tab" data-sky="off">off</button>
            <button type="button" role="tab" data-sky="on">on</button>
          </div>
        </div>
        <div class="vw-toolbar-row">
          <label class="vw-toolbar-label">Fog</label>
          <div class="vw-pill" role="tablist">
            <button type="button" role="tab" data-fog="off">off</button>
            <button type="button" role="tab" data-fog="on">on</button>
          </div>
        </div>
        <div class="vw-toolbar-row">
          <label class="vw-toolbar-label">Axes</label>
          <div class="vw-pill" role="tablist">
            <button type="button" role="tab" data-axes="off">off</button>
            <button type="button" role="tab" data-axes="on">on</button>
          </div>
        </div>
        <div class="vw-toolbar-row">
          <label class="vw-toolbar-label">Grid</label>
          <div class="vw-pill" role="tablist">
            <button type="button" role="tab" data-grid="off">off</button>
            <button type="button" role="tab" data-grid="on">on</button>
          </div>
        </div>
        <div class="vw-toolbar-section">
          <div class="vw-toolbar-section-title">Performance</div>
          <div class="vw-toolbar-row vw-perf-row">
            <div class="vw-perf-host"></div>
          </div>
          <div class="vw-toolbar-row vw-perf-info-row">
            <pre class="vw-perf-info mono"></pre>
          </div>
        </div>
        <div class="vw-toolbar-section ${this.sceneState ? '' : 'is-hidden'}">
          <div class="vw-toolbar-section-title">Sun</div>
          <div class="vw-toolbar-row">
            <label class="vw-toolbar-label" for="vw-sun-m">Month</label>
            <input id="vw-sun-m" class="vw-sun-m" type="range" min="1" max="12" step="1" />
            <span class="vw-toolbar-value mono"></span>
          </div>
          <div class="vw-toolbar-row">
            <label class="vw-toolbar-label" for="vw-sun-d">Day</label>
            <input id="vw-sun-d" class="vw-sun-d" type="range" min="1" max="28" step="1" />
            <span class="vw-toolbar-value mono"></span>
          </div>
          <div class="vw-toolbar-row">
            <label class="vw-toolbar-label" for="vw-sun-t">Time</label>
            <input id="vw-sun-t" class="vw-sun-t" type="range" min="0" max="143" step="1" />
            <span class="vw-toolbar-value mono"></span>
          </div>
        </div>
        <div class="vw-toolbar-section vw-section-layers ${this.sceneState ? '' : 'is-hidden'}">
          <div class="vw-toolbar-section-title-row">
            <span class="vw-toolbar-section-title">Layers</span>
            <span class="vw-leaf-count">0</span>
          </div>
          <div class="vw-toolbar-actions">
            <button type="button" class="vw-mini-btn" data-act="hide-all">Hide all</button>
            <button type="button" class="vw-mini-btn" data-act="show-all">Show all</button>
            <button type="button" class="vw-mini-btn" data-act="defaults">Defaults</button>
          </div>
          <input type="text" class="vw-filter" placeholder="Filter layers…" />
          <div class="vw-tree" role="tree"></div>
          <div class="vw-tree-empty">no layers — run a pipeline to populate</div>
        </div>
      </div>
    `;
    this.host.appendChild(root);
    this.root = root;
    // $btn / popover toggle are non-inline-only; bound below.
    this.$popover = root.querySelector('.vw-toolbar-popover');
    this.$pointSize = root.querySelector('#vw-point-size');
    this.$pointSizeValue = this.$pointSize.parentElement.querySelector('.vw-toolbar-value');
    // Performance section — caller populates `.perfHost` with stats.js
    // (or any other widget) and writes to `.perfInfo` for the static
    // readout. Exposed so wiring lives in lab-ui app.js, not here.
    this.perfHost = root.querySelector('.vw-perf-host');
    this.perfInfo = root.querySelector('.vw-perf-info');

    if (this.sceneState) {
      this.$sunM  = root.querySelector('.vw-sun-m');
      this.$sunD  = root.querySelector('.vw-sun-d');
      this.$sunT  = root.querySelector('.vw-sun-t');
      this.$sunMVal = this.$sunM.parentElement.querySelector('.vw-toolbar-value');
      this.$sunDVal = this.$sunD.parentElement.querySelector('.vw-toolbar-value');
      this.$sunTVal = this.$sunT.parentElement.querySelector('.vw-toolbar-value');
      this.$tree     = root.querySelector('.vw-tree');
      this.$treeEmpty = root.querySelector('.vw-tree-empty');
      this.$filter   = root.querySelector('.vw-filter');
      this.$leafCount = root.querySelector('.vw-leaf-count');
    }

    // Inline-mode split: AFTER all querySelector lookups capture refs,
    // relocate the Layers `.vw-toolbar-section` to the column's
    // dedicated `layersHost` so a column-level `.pane-header` reading
    // "Layers" sits ABOVE the visibility tree (same style as every
    // other section title in the lab). Element references stay valid;
    // we're only moving the subtree, not re-creating it.
    if (this.inline && this.layersHost) {
      const layersSection = root.querySelector('.vw-section-layers');
      if (layersSection) {
        const titleRow = layersSection.querySelector('.vw-toolbar-section-title-row');
        if (titleRow) titleRow.remove();
        this.layersHost.appendChild(layersSection);
      }
    }

    if (!this.inline) {
      this.$btn = root.querySelector('.vw-toolbar-btn');
      this.$btn.addEventListener('click', () => this._togglePopover());
      document.addEventListener('click', (ev) => {
        if (!this.root.contains(ev.target)) this._setPopover(false);
      });
    }

    // Pills.
    for (const btn of root.querySelectorAll('[data-value]')) {
      btn.addEventListener('click', () => this.setProjection(btn.dataset.value));
    }
    for (const btn of root.querySelectorAll('[data-compass]')) {
      btn.addEventListener('click', () => {
        if (btn.disabled) return;
        this.setCompass(btn.dataset.compass === 'on');
      });
    }
    for (const btn of root.querySelectorAll('[data-sky]')) {
      btn.addEventListener('click', () => this.setSky(btn.dataset.sky === 'on'));
    }
    for (const btn of root.querySelectorAll('[data-fog]')) {
      btn.addEventListener('click', () => this.setFog(btn.dataset.fog === 'on'));
    }
    for (const btn of root.querySelectorAll('[data-axes]')) {
      btn.addEventListener('click', () => this.setAxes(btn.dataset.axes === 'on'));
    }
    for (const btn of root.querySelectorAll('[data-grid]')) {
      btn.addEventListener('click', () => this.setGrid(btn.dataset.grid === 'on'));
    }

    // Point-size slider.
    this.$pointSize.value = String(this.state.pointSize);
    this.$pointSizeValue.textContent = formatSize(this.state.pointSize);
    this.$pointSize.addEventListener('input', (ev) => {
      this.setPointSize(+ev.target.value);
    });

    // Sun + layer wiring (sceneState-dependent). Time slider is packed:
    // slot = hour * 6 + minute / 10 (range 0..143, 144 ten-minute slots
    // per day). Same packing the legacy SDK's controls panel used.
    if (this.sceneState) {
      const applySun = () => {
        const m  = parseInt(this.$sunM.value, 10) || 6;
        const d  = parseInt(this.$sunD.value, 10) || 15;
        const slot = parseInt(this.$sunT.value, 10) || 0;
        const hour   = Math.floor(slot / 6);
        const minute = (slot % 6) * 10;
        this.$sunMVal.textContent = String(m);
        this.$sunDVal.textContent = String(d);
        this.$sunTVal.textContent = formatTime(slot);
        this.sceneState.sun.setFromDate({
          year: this._sunYear,
          month: m, day: d, hour, minute,
        });
      };
      [this.$sunM, this.$sunD, this.$sunT].forEach(el => {
        el.addEventListener('input', applySun);
      });

      this.$filter.addEventListener('input', (e) => {
        this._filterText = e.target.value.toLowerCase();
        this._scheduleRenderTree();
      });

      // The Hide all / Show all / Defaults buttons live inside the
      // Layers section. In inline-with-layersHost mode, that section
      // gets relocated to layersHost AFTER `_build()` finishes, so
      // querying `root` for them returns nothing — that's why the
      // buttons silently lost their click handlers. Search both root
      // and layersHost so the bind works in either layout.
      const actScope = this.layersHost ?? root;
      actScope.querySelectorAll('[data-act]').forEach(btn => {
        btn.addEventListener('click', () => {
          const a = btn.dataset.act;
          if (a === 'hide-all') this.sceneState.visibility.hideAll();
          if (a === 'show-all') this.sceneState.visibility.showAll();
          if (a === 'defaults') this.sceneState.visibility.setDefaults();
        });
      });
    }

    this._syncPills();
  }

  _togglePopover() { this._setPopover(this.$popover.hasAttribute('hidden')); }
  _setPopover(open) {
    if (open) this.$popover.removeAttribute('hidden');
    else this.$popover.setAttribute('hidden', '');
    this.$btn.setAttribute('aria-expanded', open ? 'true' : 'false');
  }

  _syncPills() {
    for (const btn of this.root.querySelectorAll('[data-value]')) {
      btn.classList.toggle('active', btn.dataset.value === this.state.projection);
    }
    for (const btn of this.root.querySelectorAll('[data-compass]')) {
      const v = btn.dataset.compass === 'on';
      btn.classList.toggle('active', v === this.state.compass);
    }
    for (const btn of this.root.querySelectorAll('[data-sky]')) {
      const v = btn.dataset.sky === 'on';
      btn.classList.toggle('active', v === this.state.sky);
    }
    for (const btn of this.root.querySelectorAll('[data-fog]')) {
      const v = btn.dataset.fog === 'on';
      btn.classList.toggle('active', v === this.state.fog);
    }
    for (const btn of this.root.querySelectorAll('[data-axes]')) {
      const v = btn.dataset.axes === 'on';
      btn.classList.toggle('active', v === this.state.axes);
    }
    for (const btn of this.root.querySelectorAll('[data-grid]')) {
      const v = btn.dataset.grid === 'on';
      btn.classList.toggle('active', v === this.state.grid);
    }
  }

  setProjection(mode) {
    this.state.projection = mode;
    this.viewer.setProjection(mode);
    this._syncPills();
    savePref(VIEWER_PREFS.projection.key, mode);
  }

  setPointSize(size) {
    this.state.pointSize = size;
    this.$pointSizeValue.textContent = formatSize(size);
    this.viewer.setPointSize(size);
    savePref(VIEWER_PREFS.pointSize.key, size);
  }

  setCompass(on) {
    this.state.compass = on;
    this.compass?.setEnabled(on);
    this._syncPills();
    savePref(VIEWER_PREFS.compass.key, on);
  }

  setSky(on) {
    this.state.sky = on;
    this.viewer.setSkyEnabled?.(on);
    this._syncPills();
    savePref(VIEWER_PREFS.sky.key, on);
  }

  setFog(on) {
    this.state.fog = on;
    this.viewer.setFogEnabled?.(on);
    this._syncPills();
    savePref(VIEWER_PREFS.fog.key, on);
  }

  setAxes(on) {
    this.state.axes = on;
    this.viewer.setAxesEnabled?.(on);
    this._syncPills();
    savePref(VIEWER_PREFS.axes.key, on);
  }

  setGrid(on) {
    this.state.grid = on;
    this.viewer.setGridEnabled?.(on);
    this._syncPills();
    savePref(VIEWER_PREFS.grid.key, on);
  }

  // ─── Visibility tree rendering ──────────────────────────────────────────

  // Coalesce tree rebuilds. A pipeline run registers hundreds of leaves
  // (one event each) plus tri-state cascades; rebuilding the whole tree per
  // event is O(N²). Here we (a) skip entirely while the Layers tree isn't
  // shown — just mark dirty; setActive(true) flushes it later — and (b)
  // collapse all events within a frame into ONE rebuild via rAF.
  _scheduleRenderTree() {
    this._treeDirty = true;
    if (!this._treeActive) return;              // results view hidden — defer
    if (this._treeRenderPending) return;
    this._treeRenderPending = true;
    requestAnimationFrame(() => {
      this._treeRenderPending = false;
      if (this._treeActive) this._renderTree();
    });
  }

  // Wired from the app's center-mode switch: `active` = the Layers tree is
  // visible (results/inspect view). Re-activating flushes any rebuild that
  // was deferred while hidden, as a single pass.
  setActive(active) {
    this._treeActive = !!active;
    if (this._treeActive && this._treeDirty) this._scheduleRenderTree();
  }

  _renderTree() {
    if (!this.sceneState) return;
    this._treeDirty = false;
    const tree = this.sceneState.visibility.tree();
    this._leafInputs.clear();
    this.$tree.innerHTML = '';
    const leafCount = countLeaves(tree);
    this.$leafCount.textContent = String(leafCount);
    this.$treeEmpty.style.display = leafCount === 0 ? '' : 'none';
    for (const child of tree.children) {
      this.$tree.appendChild(this._renderNode(child, [], 0));
    }
  }

  _renderNode(node, parentPath, depth) {
    const path = node.id ? [...parentPath, node.id] : parentPath;
    return node.isLeaf ? this._renderLeaf(node, path, depth) : this._renderGroup(node, path, depth);
  }

  _renderGroup(node, path, depth) {
    const wrap = document.createElement('div');
    wrap.className = 'vw-node vw-group';
    // No per-depth padding: indentation comes entirely from each
    // `.vw-children`'s margin/padding, so per-level indent is constant.

    const hdr = document.createElement('div');
    hdr.className = 'vw-row vw-group-row';
    const arrow = document.createElement('span');
    arrow.className = 'vw-tri-arrow';
    arrow.textContent = '▸';
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.className = 'vw-cb vw-master';
    applyTriState(cb, node.visibleDerived);
    cb.addEventListener('click', (e) => e.stopPropagation());
    cb.addEventListener('change', () => this.sceneState.visibility.setVisible(path, cb.checked));
    const label = document.createElement('span');
    label.className = 'vw-label';
    label.textContent = node.label || node.id || '(group)';
    hdr.append(arrow, cb, label);
    wrap.appendChild(hdr);

    const children = document.createElement('div');
    children.className = 'vw-children';
    children.style.display = 'none';
    for (const c of node.children) {
      children.appendChild(this._renderNode(c, path, depth + 1));
    }
    wrap.appendChild(children);

    const pathKey = path.join('/');
    let open = this._openPaths.has(pathKey);
    const setOpen = (v) => {
      open = v;
      children.style.display = open ? '' : 'none';
      arrow.textContent = open ? '▾' : '▸';
      if (open) this._openPaths.add(pathKey);
      else      this._openPaths.delete(pathKey);
    };
    setOpen(open); // apply initial DOM state from persisted set
    hdr.addEventListener('click', () => setOpen(!open));

    if (this._filterText) {
      const matches = subtreeMatches(node, this._filterText);
      wrap.style.display = matches ? '' : 'none';
      if (matches) setOpen(true);
    }

    return wrap;
  }

  _renderLeaf(node, path, depth) {
    const wrap = document.createElement('div');
    wrap.className = 'vw-node vw-leaf';
    // Indent comes from `.vw-children` (see _renderGroup) — no per-
    // depth padding here, matching the group row above.

    const row = document.createElement('label');
    row.className = 'vw-row vw-leaf-row';
    // Status dot reflects whether the leaf has any THREE.Object attached.
    // Green = ≥1 attached object (geometry in scene). Amber = leaf
    // declared but no object (visualizer hit an early-exit / produced
    // empty data — was the silent failure-mode the user hit). Tooltip
    // (native title=) carries the count.
    const status = document.createElement('span');
    status.className = 'vw-leaf-status';
    const empty = !node.objectCount || node.objectCount === 0;
    status.dataset.status = empty ? 'empty' : 'rendered';
    status.title = empty
      ? `${node.label || node.id} — empty (visualizer registered the leaf but attached no Three.js object)`
      : `${node.label || node.id} — rendered (${node.objectCount} object${node.objectCount > 1 ? 's' : ''})`;
    // Empty triangle-width spacer so leaf names align horizontally with
    // the group rows above them.
    const spacer = document.createElement('span');
    spacer.className = 'vw-tri-arrow vw-tri-spacer';
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.className = 'vw-cb';
    cb.checked = node.visible;
    cb.addEventListener('change', () => this.sceneState.visibility.setVisible(node.id, cb.checked));
    this._leafInputs.set(node.id, cb);

    const label = document.createElement('span');
    label.className = 'vw-label';
    label.textContent = node.label || node.id;
    const count = document.createElement('span');
    count.className = 'vw-count';
    if (node.objectCount > 1) count.textContent = `(${node.objectCount})`;

    // Provenance tag — distinguishes layers attached by composite
    // visualizers from those attached by per-artifact step viz. Helps
    // when the same logical layer can come from either path.
    const tag = document.createElement('span');
    tag.className = 'vw-leaf-tag';
    if (node.source === 'composer') {
      tag.textContent = 'C';
      tag.dataset.source = 'composer';
      tag.title = 'composer — attached by a composite visualizer (multi-artifact)';
    } else if (node.source === 'inspector') {
      tag.textContent = 'A';
      tag.dataset.source = 'inspector';
      tag.title = 'artifact — attached by a per-artifact step visualizer';
    }

    // Status dot lives at the END of the row — keeping it on the left
    // pushed leaf labels horizontally past their parent group's label
    // by ~11 px (dot width + margin) so same-depth leaves and groups
    // looked mis-aligned. After the tag the alignment is restored, and
    // the dot still gets read alongside the row's other badges.
    row.append(spacer, cb, label, count, tag, status);
    wrap.appendChild(row);

    if (this._filterText) {
      const haystack = [...path, node.label || '', node.id].join(' ').toLowerCase();
      wrap.style.display = haystack.includes(this._filterText) ? '' : 'none';
    }

    return wrap;
  }

  _reflectLeaf({ nodeId, visible }) {
    const cb = this._leafInputs.get(nodeId);
    if (cb) cb.checked = visible;
    this._renderTree();   // re-derive group masters
  }

  _reflectSun(state) {
    const d = state?.date;
    if (!d) return;
    if (this.$sunM && +this.$sunM.value !== d.month) {
      this.$sunM.value = d.month; this.$sunMVal.textContent = String(d.month);
    }
    if (this.$sunD && +this.$sunD.value !== d.day) {
      this.$sunD.value = d.day; this.$sunDVal.textContent = String(d.day);
    }
    const slot = d.hour * 6 + Math.floor((d.minute ?? 0) / 10);
    if (this.$sunT && +this.$sunT.value !== slot) {
      this.$sunT.value = slot; this.$sunTVal.textContent = formatTime(slot);
    }
    this._sunYear = d.year ?? this._sunYear;
  }
}

function formatSize(n) {
  // World-unit size (metres) — sizeAttenuation:true on PointsMaterial.
  const v = Number(n);
  return v >= 1 ? `${v.toFixed(1)} m` : `${(v * 100).toFixed(1)} cm`;
}

function formatTime(slot) {
  const h = Math.floor(slot / 6);
  const m = (slot % 6) * 10;
  return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}`;
}

function applyTriState(cb, derived) {
  if (derived === 'all')        { cb.checked = true;  cb.indeterminate = false; }
  else if (derived === 'none')  { cb.checked = false; cb.indeterminate = false; }
  else                          { cb.checked = false; cb.indeterminate = true;  }
}

function countLeaves(node) {
  if (node.isLeaf) return 1;
  let n = 0;
  for (const c of node.children ?? []) n += countLeaves(c);
  return n;
}

function subtreeMatches(node, needle) {
  if (node.isLeaf) {
    return `${node.label ?? ''} ${node.id ?? ''}`.toLowerCase().includes(needle);
  }
  if ((node.label ?? node.id ?? '').toLowerCase().includes(needle)) return true;
  for (const c of node.children ?? []) {
    if (subtreeMatches(c, needle)) return true;
  }
  return false;
}

const GEAR_SVG = `
<svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
  <path fill="currentColor"
    d="M8 4.754a3.246 3.246 0 100 6.492 3.246 3.246 0 000-6.492zM5.754 8a2.246 2.246 0 114.492 0 2.246 2.246 0 01-4.492 0z" />
  <path fill="currentColor"
    d="M9.796 1.343c-.527-1.79-3.065-1.79-3.592 0l-.094.319a.873.873 0 01-1.255.52l-.292-.16c-1.64-.892-3.434.902-2.54 2.541l.159.292a.873.873 0 01-.52 1.255l-.319.094c-1.79.527-1.79 3.065 0 3.592l.319.094a.873.873 0 01.52 1.255l-.16.292c-.892 1.64.901 3.434 2.541 2.54l.292-.159a.873.873 0 011.255.52l.094.319c.527 1.79 3.065 1.79 3.592 0l.094-.319a.873.873 0 011.255-.52l.292.16c1.64.893 3.434-.902 2.54-2.541l-.159-.292a.873.873 0 01.52-1.255l.319-.094c1.79-.527 1.79-3.065 0-3.592l-.319-.094a.873.873 0 01-.52-1.255l.16-.292c.893-1.64-.902-3.433-2.541-2.54l-.292.159a.873.873 0 01-1.255-.52l-.094-.319zm-2.633.283c.246-.835 1.428-.835 1.674 0l.094.319a1.873 1.873 0 002.693 1.115l.291-.16c.764-.415 1.6.42 1.184 1.185l-.159.292a1.873 1.873 0 001.116 2.692l.318.094c.835.246.835 1.428 0 1.674l-.319.094a1.873 1.873 0 00-1.115 2.693l.16.291c.415.764-.42 1.6-1.185 1.184l-.292-.159a1.873 1.873 0 00-2.692 1.116l-.094.318c-.246.835-1.428.835-1.674 0l-.094-.319a1.873 1.873 0 00-2.693-1.115l-.291.16c-.764.415-1.6-.42-1.184-1.185l.159-.291A1.873 1.873 0 001.945 8.93l-.319-.094c-.835-.246-.835-1.428 0-1.674l.319-.094A1.873 1.873 0 003.06 4.377l-.16-.292c-.415-.764.42-1.6 1.185-1.184l.292.159a1.873 1.873 0 002.692-1.115l.094-.319z" />
</svg>
`;
