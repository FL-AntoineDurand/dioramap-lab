// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// RenderContext — the five-surface API handed to every renderer module.
//
// Per §4.3 of PLAN-LAB-UI-PORT.md. Renderer modules never touch Three.js or
// the overlay DOM directly; they describe what they want via the surfaces
// below and the UI compiles it.
//
// L8 — also bridges 3D layers into `@dioramap/scene-base`'s SceneState
// (visibility tree + tri-state cascade + events) so a future
// `3DSceneStatePanel` (L9) can render the panel against a populated tree.
// This is the lab-ui-flavoured RenderContext (vs the SDK's; both
// implement the same composer-facing shape — see PLAN-VIEWER-SDK-PORT
// §4.3 for the dual-host contract).

export class RenderContext {
  // `viewer` — instance of @dioramap/scene-base's Viewer
  // `sceneState` — SceneState (optional; null in tests / pre-L8 paths)
  // `overlay` — overlay panel controller (addRender / removeRender)
  // `layerId` — stable id for this artifact, used as Three.js layer key,
  //             overlay panel entry id, AND visibility-tree leaf id base
  constructor({ viewer, sceneState, overlay, layerId, label, inspector, legendOverlay }) {
    this.viewer = viewer;
    this.sceneState = sceneState ?? null;
    this.overlay = overlay;
    this.layerId = layerId;
    this.label = label;
    this.inspector = inspector;
    this.legendOverlay = legendOverlay ?? null;
    this._cleanups = [];
    this._viewer3DLayerIds = [];
  }

  // Access a sibling artifact's decoded data (e.g. lidar-normals → positions,
  // hs-lambert-grid → hs-lambert-peaks). Returns undefined if the
  // sibling hasn't arrived yet on this session — but this call also
  // registers the request, so when the sibling later arrives the inspector
  // will destroy this context and re-fire mod.render() with the now-
  // available data. Lets visualizers be order-independent without
  // explicit subscription bookkeeping.
  getSiblingDecoded(name) {
    return this.inspector?.getSiblingDecodedFor(this.layerId, name);
  }

  // Remove a sibling's 3D layer so multiple renderers sharing the same
  // visual (e.g. positions as blue, then re-rendered colored-by-normals)
  // don't overlap on the viewer. The sibling's overlay entry stays intact.
  removeSibling3DLayer(name) {
    const siblingLayerId = this.inspector?.getLayerIdByArtifact(name);
    if (!siblingLayerId) return;
    const siblingCtx = this.inspector.contexts.get(siblingLayerId);
    // RenderContext internally tracks its 3D layer ids with a `#N` suffix.
    // Remove each of them from the viewer directly.
    for (const id of (siblingCtx?._viewer3DLayerIds ?? [])) {
      this.viewer.removeLayer(id);
    }
    siblingCtx?._viewer3DLayerIds?.splice(0);
  }

  // 3D layers. Accepts either a single primitive spec object or an array.
  // Each primitive is { kind, ...props, layer?: {id, label, path, defaultVisible} }.
  // The optional `layer` object follows the composer contract from
  // VISUALIZERS.md §4 — when present, its `id` becomes the stable
  // visibility-tree leaf id (instead of the auto-generated
  // `${ctx.layerId}#${i}`), `path` places the leaf under group nodes,
  // etc. Returns an array of the resolved layer ids so streaming
  // renderers can feed them back into viewer.growPoints() on chunks.
  render3D(specs) {
    const list = Array.isArray(specs) ? specs : [specs];
    const createdIds = [];
    list.forEach((spec, i) => {
      const layerInfo = spec.layer ?? {};
      const id = layerInfo.id ?? `${this.layerId}#${i}`;
      const isLazy = typeof spec.build === 'function';
      // Track for cleanup (idempotent — repeated render3D with same
      // stable id pushes duplicates that just get cleared together).
      this._viewer3DLayerIds.push(id);
      if (isLazy) {
        if (!this._lazyLayerIds) this._lazyLayerIds = new Set();
        this._lazyLayerIds.add(id);
      }
      createdIds.push(id);
      this.viewer.addLayer(id, spec);

      // L8 SceneState bridge: declare + attach the layer in the
      // visibility tree (gear → Layers) so the user can toggle via
      // tri-state cascades. For eager layers the object is already built
      // and we attach it now. For lazy layers we attach `null` — the
      // visibility-changed listener below builds on toggle-on and
      // disposes on toggle-off. If the leaf's defaultVisible is true,
      // we still pay the build right after attach (probe + ensureBuilt)
      // so the user sees content without having to toggle.
      if (this.sceneState) {
        const obj = isLazy ? null : (this.viewer?.layers?.get(id)?.object ?? null);
        if (obj || isLazy) {
          const leafLabel = layerInfo.label ?? (list.length > 1
            ? `${this.label} #${i}`
            : (spec?.label ?? this.label));
          this.sceneState.visibility.attach(id, obj, {
            label: leafLabel,
            path: layerInfo.path ?? spec?.path ?? [],
            defaultVisible: layerInfo.defaultVisible ?? spec?.defaultVisible ?? true,
            // Inspector = per-artifact step viz (vs `composer` which
            // lab-ui's Scene-Controls panel renders with a different
            // tag).
            source: 'inspector',
          });
          if (isLazy) {
            // If the leaf starts visible (defaultVisible: true OR a
            // previous toggle set it on before our visibility listener
            // was wired), pay the build cost now so the user sees the
            // layer without having to toggle. Otherwise wait for the
            // toggle-on event.
            const node = this.sceneState.visibility.node?.(id);
            if (node?.visible) {
              const built = this.viewer.ensureBuilt(id);
              if (built) built.visible = true;
            }
          }
        }
      }
    });

    // Wire the visibility listener ONCE per context — NOT per render3D
    // batch. A single roof-graph artifact calls render3D dozens of times
    // (one per layer kind, some per-plane); a listener pair per batch piled
    // up O(render3D-calls) listeners that every visibility event then fanned
    // out across (the _onEventListenerChange churn in the profile). The
    // shared handler dispatches by id against this context's cumulative lazy
    // set (this._lazyLayerIds), which is equivalent since ensureBuilt /
    // disposeBuilt key off the id regardless of which batch declared it.
    if (this.sceneState && this._lazyLayerIds?.size > 0 && !this._visListenerWired) {
      this._visListenerWired = true;
      const handleLeaf = (nodeId, visible) => {
        if (!this._lazyLayerIds?.has(nodeId)) return;
        if (visible) {
          const built = this.viewer.ensureBuilt(nodeId);
          if (built) built.visible = true;
        } else {
          this.viewer.disposeBuilt(nodeId);
        }
      };
      const onVis = (e) => handleLeaf(e.detail.nodeId, e.detail.visible);
      const onSubtree = (e) => {
        for (const { nodeId, visible } of e.detail.affectedLeaves) {
          handleLeaf(nodeId, visible);
        }
      };
      this.sceneState.addEventListener('visibility-changed', onVis);
      this.sceneState.addEventListener('subtree-changed',    onSubtree);
      this._cleanups.push(() => {
        this.sceneState.removeEventListener('visibility-changed', onVis);
        this.sceneState.removeEventListener('subtree-changed',    onSubtree);
        this._visListenerWired = false;
      });
    }

    // Tag this artifact's overlay entry so the layer-list row shows the 3D
    // icon; no block is appended to the stack for a pure-3D render.
    this.overlay.appendRender(this.layerId, '3d', {});
    this._cleanups.push(() => {
      for (const id of this._viewer3DLayerIds) this.viewer.removeLayer(id);
      // Drop the corresponding leaves from the visibility tree too.
      if (this.sceneState) {
        for (const id of this._viewer3DLayerIds) {
          try { this.sceneState.visibility.clearSubtree(id); }
          catch (_) { /* ignore — leaf may already be gone */ }
        }
      }
      this._viewer3DLayerIds.length = 0;
      this._lazyLayerIds?.clear();
    });
    return createdIds;
  }

  // Overlay panel surfaces. Each call appends one "block" to the overlay
  // entry for this artifact; multiple calls stack.
  renderSVG({ name, gElement, width, height }) {
    this.overlay.appendRender(this.layerId, 'svg', { name, gElement, width, height });
  }

  renderChart(builderFn) {
    this.overlay.appendRender(this.layerId, 'chart', { build: builderFn });
  }

  renderJSON(value) {
    this.overlay.appendRender(this.layerId, 'json', { value });
  }

  renderHTML(elementOrString) {
    this.overlay.appendRender(this.layerId, 'html', { content: elementOrString });
  }

  // Register a legend (Phase 5.2-followup-A). The legend overlay anchors
  // it to a 3D layer id so the legend hides/shows automatically when the
  // user toggles that layer in the visibility tree. If `targetLayerId`
  // is omitted, the overlay associates with this ctx's primary layerId
  // (works for single-layer step-vizes; multi-layer ones should pass
  // explicitly so each layer's legend toggles correctly).
  // spec: see apps/lab-ui/src/viewer/legend-overlay.js — {title,
  //       swatches?, gradient?, note?}
  renderLegend(spec, targetLayerId) {
    if (!this.legendOverlay) return;
    const layerId = targetLayerId
      ?? this._viewer3DLayerIds[0]
      ?? this.layerId;
    this.legendOverlay.registerLegend(layerId, spec, this.layerId);
    this._cleanups.push(() => this.legendOverlay.unregisterLegend(layerId));
  }

  onCleanup(fn) { this._cleanups.push(fn); }

  // Called by artifact-inspector when the user removes this layer.
  destroy() {
    for (const fn of this._cleanups) {
      try { fn(); } catch (e) { console.warn('[ctx cleanup]', e); }
    }
    this._cleanups.length = 0;
    this.overlay.removeEntry(this.layerId);
  }
}
