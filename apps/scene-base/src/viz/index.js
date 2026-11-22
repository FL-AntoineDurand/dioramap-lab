// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// viz/index.js — Vite-glob discovery entry for lab-ui.
//
// IMPORTANT: this module is **lab-ui only**. The SDK never imports it
// (PLAN-VIEWER-SDK-PORT §5.8) — Vite-glob with `eager: true` would pull
// every viz module into the SDK's bundle, defeating tree-shaking.
// The SDK imports composers explicitly per §5.2.
//
// Lab-ui consumes `vizCatalog` and filters by `meta.requires` against the
// pipeline CATALOG response. Each entry is one composer, ready to render.

const _all = import.meta.glob('./*.viz.js', { eager: true });

export const vizCatalog = Object.entries(_all).map(([path, mod]) => {
  const id = path.replace(/^\.\//, '').replace(/\.viz\.js$/, '');
  return {
    id,                  // e.g. 'parcel-lines'
    meta: mod.meta ?? {},
    module: mod,         // exposes render, optional begin/append/finalizeStream, dispose
  };
});

/**
 * Filter the catalog to entries whose `meta.requires` are all present in
 * `availableArtifacts` (a Set of artifact names). Convenience for lab-ui's
 * "Composite views" panel.
 */
export function filterByCatalog(availableArtifacts) {
  return vizCatalog.filter((entry) => {
    const reqs = entry.meta?.requires ?? [];
    return reqs.every((r) => availableArtifacts.has(r));
  });
}
