// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// @dioramap/scene-base — public re-exports for the most-used surfaces.
//
// Consumers can import via the package root for the common path, or via
// per-file subpaths (declared in package.json `exports`) when tree-shaking
// matters. The viewer-sdk uses subpaths (closed allow-list of composers);
// lab-ui mostly uses the root + viz/ Vite glob (separate entry, not yet
// shipped — lands at S4).

export { Viewer, setEdlOptIn } from './scene/create-scene.js';
export { MarineCompass } from './scene/compass-gizmo.js';
export { createEDLPass } from './scene/postprocess/edl.js';
export { BaseRenderContext } from './scene/render-context.js';
export { SceneState, createSceneState } from './scene/scene-state.js';
export { SunControl } from './scene/sun.js';
export { generateLeafTexture } from './utilities/leaf-sprite.js';
export { loadGLBModel } from './utilities/gltf-loader.js';
