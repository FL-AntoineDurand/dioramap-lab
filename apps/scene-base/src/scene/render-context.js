// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// RenderContext — what viz modules see at render time.
//
// PLAN-VIEWER-SDK-PORT §4.2: composers call `ctx.render3D(specs)` and the
// ctx implementation handles the host-specific dual effect:
//
//   Lab-ui ctx.render3D:
//     1. add Three.js objects to the scene graph (viewer.addLayer)
//     2. declare + attach in sceneState.visibility (auto-registration)
//     3. mark the artifact's OverlayPanel row with the 3D surface icon
//
//   SDK ctx.render3D:
//     1. add Three.js objects to the scene graph
//     2. declare + attach in sceneState.visibility
//     (no inspector → no overlay-panel side effect)
//
// The base class here covers (1) and (2). Hosts subclass to add (3) or
// any extra side effects.
//
// Non-3D surface methods (renderSVG/Chart/JSON/HTML) are no-ops in the
// base; lab-ui overrides them to feed its OverlayPanel. Composers are
// host-agnostic — they just call ctx.render*; non-applicable surfaces
// silently drop in the SDK.

/**
 * Spec accepted by render3D:
 *   {
 *     kind: 'points' | 'mesh' | 'lines' | 'instanced-mesh' | 'group' | ...
 *     object: THREE.Object3D,
 *     layer: { id, label?, path?, defaultVisible? }
 *   }
 *
 * For a single composer call, `specs` may be a single spec or an array.
 * Each spec attaches its `object` to the layer named by `layer.id` —
 * multiple objects can share a layer if a composer wants to group them
 * under one toggle.
 */
export class BaseRenderContext {
  /**
   * @param {Object} args
   * @param {Object} args.viewer       — Viewer instance with addLayer/removeLayer
   * @param {Object} args.sceneState   — SceneState (S2). May be null in tests.
   * @param {string} args.layerId      — artifact-id-derived stable id for cleanup
   * @param {string} args.label        — human label (used as fallback if layer.label absent)
   */
  constructor({ viewer, sceneState, layerId, label }) {
    this.viewer = viewer;
    this.sceneState = sceneState;
    this.layerId = layerId;
    this.label = label;
    this._cleanups = [];
    this._declaredLayers = new Set();
  }

  /**
   * Add Three.js objects + auto-register their layer with sceneState.
   * Composers should always call this rather than viewer.addLayer directly.
   */
  render3D(specs) {
    const list = Array.isArray(specs) ? specs : [specs];
    for (const spec of list) {
      if (!spec || !spec.object) continue;
      const layerInfo = spec.layer ?? {};
      // Default the layer id to the ctx's artifact-derived id so simple
      // (single-layer) composers don't have to declare anything.
      const lid = layerInfo.id ?? this.layerId;
      const lbl = layerInfo.label ?? this.label;
      const path = layerInfo.path ?? [];
      const defaultVisible = layerInfo.defaultVisible ?? true;

      // Register with sceneState first so the auto-declare emits
      // 'visibility-registered' before the object is attached.
      if (this.sceneState) {
        this.sceneState.visibility.attach(lid, spec.object, {
          label: lbl,
          path,
          defaultVisible,
          // BaseRenderContext is the scene-base composer-side context
          // (consumed by CompositeViewsDispatcher in lab-ui and by the
          // SDK orchestrator). Tag every leaf attached here as composer-
          // origin so the visibility tree UI can flag it.
          source: 'composer',
        });
        this._declaredLayers.add(lid);
      }

      // Add to the scene graph via viewer.addLayer if available.
      // Hosts that don't use the addLayer API can override render3D.
      if (this.viewer && typeof this.viewer.addLayer === 'function') {
        // viewer.addLayer signature is host-defined; pass the spec through.
        this.viewer.addLayer(`${this.layerId}::${lid}`, spec);
      }
    }
  }

  // S1+ stubs — non-3D surfaces no-op by default; lab-ui overrides.
  renderSVG(_arg) {}
  renderChart(_builderFn) {}
  renderJSON(_value) {}
  renderHTML(_elementOrString) {}

  // Cleanup callback registry — called by dispose() in declared order.
  onCleanup(fn) {
    if (typeof fn === 'function') this._cleanups.push(fn);
  }

  /**
   * Tear down everything this ctx contributed: declared layers (in
   * sceneState), Three.js objects (via viewer.removeLayer), and
   * registered cleanup callbacks.
   */
  dispose() {
    if (this.viewer && typeof this.viewer.removeLayer === 'function') {
      for (const lid of this._declaredLayers) {
        try { this.viewer.removeLayer(`${this.layerId}::${lid}`); }
        catch (e) { console.warn('[RenderContext.dispose viewer]', e); }
      }
    }
    if (this.sceneState) {
      for (const lid of this._declaredLayers) {
        try { this.sceneState.visibility.clearSubtree(lid); }
        catch (e) { console.warn('[RenderContext.dispose visibility]', e); }
      }
    }
    this._declaredLayers.clear();
    for (const fn of this._cleanups) {
      try { fn(); }
      catch (e) { console.warn('[RenderContext.dispose cleanup]', e); }
    }
    this._cleanups.length = 0;
  }
}
