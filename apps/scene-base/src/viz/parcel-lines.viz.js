// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand

const t = (s) => s;

// parcel-lines.viz.js — single-artifact viz for cadastre parcel boundaries.
//
// Reads:
//   parcel-lines  AK_POINT_COLUMN — N segments, each 6 F32 in scene-local
//                 v3 Z-up frame (Lambert-93 native, recentered by scene-
//                 origin server-side). Same coord system as recentered
//                 lidar-positions, so renders directly with no axis swap.
//                 (build-parcel-lines step v2+; v1 emitted Y-up.)
//   scene-origin  OPAQUE_JSON — kept as a `requires` for ordering only;
//                 the actual position math is server-side.
//
// Renders one THREE.LineSegments under the layer id `parcels`. Yellow
// (legacy SDK colour, slightly toned). No LineMaterial-based fat lines yet
// (the legacy SDK used three/addons/lines/* for screen-space thickness — V3
// can add that as a polish later; here we use plain THREE.LineSegments).

import * as THREE from 'three';

export const meta = {
  name: 'Cadastre — parcel boundaries',
  requires: ['parcel-lines', 'scene-origin'],
  pipelines: '*',
  default: true,
};

export function render(ctx, decoded) {
  const segPayload = decoded['parcel-lines'];
  if (!segPayload?.data) return;

  // Decoded shape (artifact-decoder.decodePointColumn with the
  // 'parcel-lines' schema entry): { dtype: 'f32', components: 6,
  // n_items: nSegments, data: Float32Array(nSegments*6) } — the
  // segments are already in scene-local Z-up metres so Three.js can
  // render them directly without any per-vertex transform.
  const positions = segPayload.data;
  const nSegments = segPayload.n_items;
  if (nSegments === 0) return;

  const geom = new THREE.BufferGeometry();
  // Each segment = 2 endpoints × 3 floats. The artifact's `data` is a
  // Float32Array of length nSegments*6 — exactly the layout
  // THREE.LineSegments expects.
  geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  const mat = new THREE.LineBasicMaterial({
    color: 0xffd700,
    transparent: true,
    opacity: 0.9,
    depthWrite: false,
  });
  const lines = new THREE.LineSegments(geom, mat);
  lines.name = 'parcel-lines';

  ctx.render3D({
    kind: 'lines',
    object: lines,
    layer: { id: 'parcels', label: 'Cadastre outlines', defaultVisible: true },
  });

  ctx.onCleanup(() => {
    geom.dispose();
    mat.dispose();
  });
}
