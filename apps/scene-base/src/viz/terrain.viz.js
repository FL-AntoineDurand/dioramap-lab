// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// t() — Phase 1 obfuscation marker; see viewer-sdk/src/viewer.js.
const t = (s) => s;

// terrain.viz.js — local lidar-ground heightmap textured with the
// deshadowed ortho.
//
// Reads:
//   lidar-ground-grid               AK_RASTER_F32, 1 channel — per-cell min Z
//   deshadowed-ortho-rgb-rasters    AK_RASTER_F32, ≥3 channels — R/G/B in 0..1
//   canonical-location              OPAQUE_JSON — {bbox_l93}
//   scene-origin                    OPAQUE_JSON — {x, y, z}
//
// One displaced mesh: PlaneGeometry sized to bbox_l93, vertex z displaced
// by sampled lidar ground; UV-mapped to the deshadowed RGB texture.
// Z-up (v3 convention) — no axis swap. Replaces the cruder
// deshadowed-ortho.viz.js flat plane (this composite has `default: true`,
// the flat plane stays as a fallback when the ground grid isn't subscribed).

import * as THREE from 'three';

export const meta = {
  name: 'Local terrain (heightmap + deshadowed ortho)',
  requires: [
    'lidar-ground-grid',
    'deshadowed-ortho-rgb-rasters',
    'canonical-location',
    'scene-origin',
  ],
  pipelines: '*',
  default: true,
};

export function render(ctx, decoded) {
  const grid = decoded['lidar-ground-grid'];
  const ortho = decoded['deshadowed-ortho-rgb-rasters'];
  const loc = decoded['canonical-location'];
  const origin = decoded['scene-origin']?.json;
  if (!grid?.channels?.length || !ortho?.channels || ortho.channels.length < 3) return;
  if (!loc?.json?.bbox_l93 || !origin) return;

  const heights = grid.channels[0];
  const { cols: gCols, rows: gRows } = grid;
  const { cols: oCols, rows: oRows, channels: oCh } = ortho;
  const [xmin, ymin, xmax, ymax] = loc.json.bbox_l93;
  const sizeX = xmax - xmin;
  const sizeY = ymax - ymin;
  const cx = (xmin + xmax) / 2 - origin.x;
  const cy = (ymin + ymax) / 2 - origin.y;

  // Ortho texture from the F32 RGB channels — clamped to U8.
  const oCanvas = document.createElement('canvas');
  oCanvas.width = oCols;
  oCanvas.height = oRows;
  const oCtx = oCanvas.getContext('2d');
  const img = oCtx.createImageData(oCols, oRows);
  const r = oCh[0], g = oCh[1], b = oCh[2];
  // Raster row 0 = ymin (south); canvas row 0 = top. PlaneGeometry's UV
  // matches canvas (v=0 at top), and the geometry's Y axis points
  // toward +north (after the translate below). Flip the source row so
  // texture-north = scene-north.
  for (let py = 0; py < oRows; py++) {
    const srcRow = oRows - 1 - py;
    for (let px = 0; px < oCols; px++) {
      const s = srcRow * oCols + px;
      const di = (py * oCols + px) * 4;
      img.data[di + 0] = clampU8(r[s]);
      img.data[di + 1] = clampU8(g[s]);
      img.data[di + 2] = clampU8(b[s]);
      img.data[di + 3] = 255;
    }
  }
  oCtx.putImageData(img, 0, 0);

  const tex = new THREE.CanvasTexture(oCanvas);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.minFilter = THREE.LinearFilter;
  tex.magFilter = THREE.LinearFilter;
  tex.needsUpdate = true;

  // Heightmap geometry: one segment per ground-grid cell.
  const geom = new THREE.PlaneGeometry(sizeX, sizeY, gCols - 1, gRows - 1);
  const pos = geom.attributes.position;
  // PlaneGeometry vertex order is row-major top-left → bottom-right (v=0
  // at the top of the geometry, which after recenter maps to +y/north
  // in our Z-up scene). Raster row 0 = ymin = south = bottom = highest
  // geometry-row index. Flip the source row so high vertices end up at
  // the right XY location.
  let minZ = Infinity, maxZ = -Infinity;
  for (let i = 0; i < pos.count; i++) {
    const x = i % gCols;
    const y = Math.floor(i / gCols);
    const srcRow = gRows - 1 - y;
    const v = heights[srcRow * gCols + x];
    const z = Number.isFinite(v) ? v - origin.z : 0;
    pos.setZ(i, z);
    if (Number.isFinite(z)) {
      if (z < minZ) minZ = z;
      if (z > maxZ) maxZ = z;
    }
  }
  pos.needsUpdate = true;
  geom.computeVertexNormals();
  geom.computeBoundingBox();
  geom.computeBoundingSphere();

  const mat = new THREE.MeshStandardMaterial({
    map: tex,
    side: THREE.DoubleSide,
    roughness: 1.0,
    metalness: 0.0,
  });
  const mesh = new THREE.Mesh(geom, mat);
  mesh.position.set(cx, cy, 0);
  mesh.name = 'local-terrain';
  mesh.receiveShadow = true;

  ctx.render3D({
    kind: 'mesh',
    object: mesh,
    layer: { id: 'terrain', label: 'Local terrain', defaultVisible: true },
  });

  ctx.onCleanup(() => {
    geom.dispose();
    mat.dispose();
    tex.dispose();
  });
}

function clampU8(v) {
  if (Number.isNaN(v)) return 0;
  const n = Math.round(v * 255);
  return n < 0 ? 0 : n > 255 ? 255 : n;
}
