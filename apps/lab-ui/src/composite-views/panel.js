// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// composite-views/panel.js — toggle list for composite-visualizers.
//
// Per VISUALIZERS.md §6.4, the composite-views panel is the *enable/
// disable* surface for composite-visualizers. Their layer-tree leaves
// surface in the gear-popover 3D scene panel like any other visualizer's
// leaves; this panel is just the on/off switch.
//
// Discovery: import vizCatalog from @dioramap/scene-base/viz (Vite glob).
// On every artifacts-loaded event for the current pipeline, rebuild the
// row list filtered by which composites' meta.requires are satisfied.
// Pre-check rows where meta.default === true.

import { vizCatalog, filterByCatalog } from '@dioramap/scene-base/viz';

export class CompositeViewsPanel {
  /**
   * @param {HTMLElement}                 host       container (right column section)
   * @param {CompositeViewsDispatcher}    dispatcher the runtime that enable/disables composites
   */
  constructor(host, { dispatcher }) {
    this.host = host;
    this.dispatcher = dispatcher;
    this._rows = new Map(); // id → { row, checkbox }
    this._availableArtifacts = null;

    this.host.classList.add('composite-views-panel');
    this._empty = document.createElement('div');
    this._empty.className = 'composite-views-empty';
    this._empty.textContent = '(no composites available — load a pipeline)';
    this.host.appendChild(this._empty);
  }

  /**
   * Called on artifacts-loaded. Computes eligible composites + auto-enables
   * the meta.default===true ones if not yet enabled.
   * @param {string[]} artifactNames — names of artifacts the pipeline declares
   */
  setAvailableArtifacts(artifactNames) {
    this._availableArtifacts = new Set(artifactNames);
    const eligible = filterByCatalog(this._availableArtifacts);
    this._render(eligible);

    // Auto-enable defaults — rows pre-checked, dispatcher fires their
    // subscribe + render when ready (or immediately if cached).
    for (const entry of eligible) {
      if (entry.meta?.default === true && !this.dispatcher.isEnabled(entry.id)) {
        this.dispatcher.enable(entry.id, entry.module);
        const row = this._rows.get(entry.id);
        if (row) row.checkbox.checked = true;
      }
    }
  }

  _render(entries) {
    this.host.innerHTML = '';
    this._rows.clear();
    if (entries.length === 0) {
      this.host.appendChild(this._empty);
      this._empty.textContent = this._availableArtifacts == null
        ? '(no composites available — load a pipeline)'
        : '(no composite-visualizers match this pipeline)';
      return;
    }
    for (const entry of entries) {
      const row = document.createElement('label');
      row.className = 'composite-views-row';
      const cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.checked = this.dispatcher.isEnabled(entry.id);
      cb.addEventListener('change', () => {
        if (cb.checked) this.dispatcher.enable(entry.id, entry.module);
        else this.dispatcher.disable(entry.id);
      });
      const label = document.createElement('span');
      label.className = 'composite-views-label';
      label.textContent = entry.meta?.name ?? entry.id;
      const requires = document.createElement('span');
      requires.className = 'composite-views-requires mono';
      const reqList = entry.meta?.requires ?? [];
      requires.textContent = reqList.length
        ? `[${reqList.join(', ')}]`
        : '';
      row.appendChild(cb);
      row.appendChild(label);
      row.appendChild(requires);
      this.host.appendChild(row);
      this._rows.set(entry.id, { row, checkbox: cb });
    }
  }
}

// Export the catalog so callers can introspect (debugging).
export { vizCatalog };
