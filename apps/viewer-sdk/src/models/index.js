// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Models system entry point — wires manager + interaction together
// and exposes the surface the SDK's public methods call into.
//
// Per-frame, mountViewer must call models.update() so the action-button
// CSS2D overlay stays positioned. We hook this into the viewer's render
// loop in viewer.js (see `renderer.setAnimationLoop` / equivalent there).

import { createModelManager } from './model-manager.js';
import { createModelInteraction } from './model-interaction.js';
import { startGhostPlacement } from './ghost-placement.js';

/**
 * @param {Object} opts
 * @param {Object} opts.viewer  — Viewer instance from @dioramap/scene-base
 *                                (exposes scene, activeCamera, renderer,
 *                                 controls, plus the per-frame render call)
 * @param {(uuid:string, info:Object)=>void} [opts.onSelected]
 * @param {(uuid:string, transform:Object)=>void} [opts.onMoved]
 * @param {()=>void} [opts.onDeselected]
 */
export function createModelsSystem({ viewer, onSelected, onMoved, onDeselected }) {
  const manager = createModelManager({ scene: viewer.scene });
  const interaction = createModelInteraction({
    scene: viewer.scene,
    camera: viewer.activeCamera,
    renderer: viewer.renderer,
    orbitControls: viewer.controls,
    modelManager: manager,
    onSelected, onMoved, onDeselected,
  });

  // Track an in-flight ghost placement so we can cancel it from outside
  // (e.g. user starts a different placement before finishing the first).
  let _ghost = null;

  return {
    // Data API.
    addModel:       (opts)  => manager.addModel(opts),
    removeModel:    (uuid)  => manager.removeModel(uuid),
    getModels:      ()      => manager.getModels(),
    getModel:       (uuid)  => manager.getModel(uuid),
    setModelTransform: (uuid, t) => manager.setTransform(uuid, t),

    /**
     * Start ghost placement: a translucent preview of the GLB follows
     * the cursor. Click to confirm (calls addModel at the cursor's
     * picked ground position); Escape (or cancelGhost()) to abort.
     * Returns a Promise that resolves to the placed model's uuid (or
     * null if cancelled).
     */
    startGhostPlace(opts) {
      if (_ghost) _ghost.cancel();
      _ghost = startGhostPlacement({
        url: opts.url,
        data: opts.data,
        scene: viewer.scene,
        camera: viewer.activeCamera,
        renderer: viewer.renderer,
        sampleGroundZ: manager.sampleGroundZ,
        modelManager: manager,
        modelOpts: opts,
      });
      const p = _ghost.promise.finally(() => { _ghost = null; });
      return p;
    },
    cancelGhost() {
      if (_ghost) { _ghost.cancel(); _ghost = null; }
    },

    // Selection API.
    selectModel:      (uuid) => interaction.select(uuid),
    deselectModel:    ()     => interaction.deselect(),
    getSelectedModel: ()     => {
      const id = interaction.getSelectedId();
      return id ? manager.getModel(id) : null;
    },
    setGizmoMode:     (mode) => interaction.setMode(mode),

    // Internals — viewer.js uses these.
    _tick: interaction.update,
    _dispose: () => {
      interaction.dispose();
      // manager has no dispose — its contents are scene-attached and
      // get released via removeModel. The viewer's overall dispose
      // calls scene.clear() which would also collect anything left.
    },
  };
}
