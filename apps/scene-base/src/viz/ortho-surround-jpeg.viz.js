// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// t() — Phase 1 obfuscation marker; see viewer-sdk/src/viewer.js.
const t = (s) => s;

// ortho-surround-jpeg.viz.js — flat surround backdrop (JPEG aerial).
//
// Reads:
//   ortho-surround-jpeg  AK_IMAGE_JPEG (raw JPEG bytes; large surround tile)
//   canonical-location   OPAQUE_JSON {bbox_l93}
//   scene-origin         OPAQUE_JSON {x, y, z}
//
// Renders the JPEG as a flat textured plane sized to canonical-location's
// bbox_l93 expanded by `meta.options.surround_padding_m` (default 200 m on
// each side — surrounding context). Sits at a deeper z than the deshadowed
// ortho plane so the deshadowed view always wins where they overlap.
//
// This is the "surround backdrop" entry from PLAN §4.1. The proper terrain
// mesh that uses this JPEG as a texture on the elevation surround lands at
// S5 (terrain.viz.js multi-artifact composer).

import * as THREE from 'three';

export const meta = {
  name: 'Surround backdrop — flat JPEG',
  requires: ['ortho-surround-jpeg', 'canonical-location', 'scene-origin'],
  pipelines: '*',
  default: false,  // S5's terrain.viz supersedes this; left as a fallback.
};

const BACKDROP_Z_OFFSET = -2.0;
const SURROUND_PADDING_M = 200;

export async function render(ctx, decoded) {
  const jpeg = decoded['ortho-surround-jpeg'];
  const loc = decoded['canonical-location'];
  const origin = decoded['scene-origin']?.json;
  if (!jpeg?.bytes || !loc?.json?.bbox_l93 || !origin) return;

  const [xmin, ymin, xmax, ymax] = loc.json.bbox_l93;
  const xMinPad = xmin - SURROUND_PADDING_M;
  const yMinPad = ymin - SURROUND_PADDING_M;
  const xMaxPad = xmax + SURROUND_PADDING_M;
  const yMaxPad = ymax + SURROUND_PADDING_M;
  const sizeX = xMaxPad - xMinPad;
  const sizeY = yMaxPad - yMinPad;
  const cx = (xMinPad + xMaxPad) / 2 - origin.x;
  const cy = (yMinPad + yMaxPad) / 2 - origin.y;

  const blob = new Blob([jpeg.bytes], { type: 'image/jpeg' });
  const bitmap = await createImageBitmap(blob);

  const texture = new THREE.CanvasTexture(bitmap);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.minFilter = THREE.LinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.needsUpdate = true;

  const geom = new THREE.PlaneGeometry(sizeX, sizeY);
  const mat = new THREE.MeshBasicMaterial({
    map: texture,
    side: THREE.DoubleSide,
    depthWrite: false,
  });
  const mesh = new THREE.Mesh(geom, mat);
  mesh.position.set(cx, cy, BACKDROP_Z_OFFSET);
  mesh.name = 'ortho-surround-jpeg';

  ctx.render3D({
    kind: 'mesh',
    object: mesh,
    layer: { id: 'surround-rgb', label: 'Surround backdrop (JPEG)', defaultVisible: true },
  });

  ctx.onCleanup(() => {
    geom.dispose();
    mat.dispose();
    texture.dispose();
    bitmap.close?.();
  });
}
