// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Mask-Render pass — renders the EDL opt-in subset of the scene into a
// single colour render target, producing a per-pixel mask used by the
// EDL pass to gate its effect.
//
// Why this exists (vs the older stencil-extract approach):
//   The previous design wrote stencil=1 in opt-in materials and either
//   (a) blitted that stencil to the canvas for a fixed-function stencil
//   test on the EDL fullscreen pass, or (b) sampled stencil bits via
//   the WEBGL_stencil_texturing extension in a usampler2D shader. (a)
//   hit ANGLE's CPU pixel-by-pixel fallback for blitFramebuffer →
//   STENCIL_BUFFER_BIT on Firefox/Windows (~100 ms/frame at 1080p).
//   (b) is fast but requires WEBGL_stencil_texturing which isn't
//   exposed on every Firefox build/profile (silent fallback to no-EDL).
//
// This approach avoids both: opt-in objects are tagged via THREE.Layers
// (mesh.layers.enable(EDL_LAYER)) and we render the scene a second time
// with `camera.layers.set(EDL_LAYER)` + `scene.overrideMaterial = red`
// to produce the mask. Cost: one extra render of the opt-in subset
// (typically a single InstancedMesh — disc-other). No extensions, no
// blits.

import * as THREE from 'three';
import { Pass } from 'three/addons/postprocessing/Pass.js';

// Layer index used to flag EDL-opt-in objects. Layer 0 is the default
// "everything renders here" layer; we pick 2 (skipping 1) to leave room
// for any future per-feature layer scheme. Materials don't carry layers
// in Three.js — the flag is on the Object3D (mesh).
export const EDL_LAYER = 2;

export class MaskRenderPass extends Pass {
  /**
   * @param {THREE.Scene} scene
   * @param {() => THREE.Camera} getCamera  Lambda — returns the current
   *   active camera. The Viewer swaps perspective ↔ ortho via setProjection
   *   and we need to track that, so we resolve fresh each frame.
   * @param {THREE.WebGLRenderTarget} target  Mask target the red flat
   *   material renders into. RGBA8 colour, depth buffer attached (for
   *   z-fight between opt-in objects). Cleared per frame.
   */
  constructor(scene, getCamera, target) {
    super();
    this._scene = scene;
    this._getCamera = getCamera;
    this._target = target;
    this.needsSwap = false;
    // Pure red, no fog, no shading. RGBA = (1,0,0,1) at every opt-in
    // pixel. EDL pass reads .r ≥ 0.5 ⇒ apply EDL. Three.js's stock
    // shader for MeshBasicMaterial supports InstancedMesh transforms
    // automatically (instance_pars_vertex / instance_vertex chunks),
    // so we don't need a custom material per mesh type.
    this._maskMaterial = new THREE.MeshBasicMaterial({
      color: 0xff0000,
      fog: false,
      transparent: false,
      depthTest: true,
      depthWrite: true,
    });
  }

  render(renderer) {
    const cam = this._getCamera();
    const prevLayersMask = cam.layers.mask;
    const prevOverride = this._scene.overrideMaterial;
    const prevTarget = renderer.getRenderTarget();
    const prevAutoClearColor = renderer.autoClearColor;
    const prevAutoClearDepth = renderer.autoClearDepth;
    const prevAutoClearStencil = renderer.autoClearStencil;
    const prevClearColor = new THREE.Color();
    renderer.getClearColor(prevClearColor);
    const prevClearAlpha = renderer.getClearAlpha();

    cam.layers.set(EDL_LAYER);              // ONLY opt-in objects render
    this._scene.overrideMaterial = this._maskMaterial;
    renderer.setRenderTarget(this._target);
    renderer.setClearColor(0x000000, 1);    // mask=0 outside opt-in pixels
    renderer.autoClearColor = true;
    renderer.autoClearDepth = true;
    renderer.autoClearStencil = false;
    renderer.render(this._scene, cam);

    cam.layers.mask = prevLayersMask;
    this._scene.overrideMaterial = prevOverride;
    renderer.setRenderTarget(prevTarget);
    renderer.setClearColor(prevClearColor, prevClearAlpha);
    renderer.autoClearColor = prevAutoClearColor;
    renderer.autoClearDepth = prevAutoClearDepth;
    renderer.autoClearStencil = prevAutoClearStencil;
  }

  dispose() {
    this._maskMaterial?.dispose?.();
  }
}
