// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand

const t = (s) => s;

// deshadowed-ortho.viz.js — single-artifact ground texture plane.
//
// Reads:
//   deshadowed-ortho-rgb-rasters  AK_RASTER_F32 (cols, rows, ≥3 channels: R,G,B,...)
//   canonical-location            OPAQUE_JSON {bbox_l93: [xmin, ymin, xmax, ymax]}
//   scene-origin                  OPAQUE_JSON {x, y, z, ...}
//
// Builds a textured plane in the XY plane at a fixed z-offset below the
// scene origin and registers it as a single visibility leaf 'ground-ortho'.
// Channels 0..2 (R, G, B) are packed into RGB pixels; the float values are
// expected to be in 0..1 (build-ortho-rgb-rasters' output convention).

import * as THREE from 'three';

export const meta = {
  name: 'Deshadowed ortho — ground plane',
  requires: ['deshadowed-ortho-rgb-rasters', 'canonical-location', 'scene-origin'],
  pipelines: '*',
  default: true,
};

// Ground plane sits 0.5 m below scene origin so it doesn't z-fight the
// real ground points (which span min_z..max_z around z=0 after recenter).
const GROUND_PLANE_Z_OFFSET = -0.5;

export function render(ctx, decoded) {
  const raster = decoded['deshadowed-ortho-rgb-rasters'];
  const loc = decoded['canonical-location'];
  const origin = decoded['scene-origin']?.json;
  if (!raster?.channels || !loc?.json?.bbox_l93 || !origin) return;

  const { cols, rows, channels } = raster;
  if (channels.length < 3) return;
  const [xmin, ymin, xmax, ymax] = loc.json.bbox_l93;

  // Build an RGBA canvas from channels 0..2. Values are F32 in 0..1; clamp
  // and convert to U8. Row 0 of the raster is at ymin (south); canvas row
  // 0 is at top (north), so flip vertically.
  const canvas = document.createElement('canvas');
  canvas.width = cols;
  canvas.height = rows;
  const c2d = canvas.getContext('2d');
  const img = c2d.createImageData(cols, rows);
  const r = channels[0], g = channels[1], b = channels[2];
  for (let y = 0; y < rows; y++) {
    const srcRow = rows - 1 - y;
    for (let x = 0; x < cols; x++) {
      const s = srcRow * cols + x;
      const p = (y * cols + x) * 4;
      img.data[p + 0] = clampU8(r[s]);
      img.data[p + 1] = clampU8(g[s]);
      img.data[p + 2] = clampU8(b[s]);
      img.data[p + 3] = 255;
    }
  }
  c2d.putImageData(img, 0, 0);

  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.minFilter = THREE.LinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.needsUpdate = true;

  const sizeX = xmax - xmin;
  const sizeY = ymax - ymin;
  const cx = (xmin + xmax) / 2 - origin.x;
  const cy = (ymin + ymax) / 2 - origin.y;

  const geom = new THREE.PlaneGeometry(sizeX, sizeY);
  const mat = new THREE.MeshBasicMaterial({
    map: texture,
    side: THREE.DoubleSide,
    transparent: false,
  });
  const mesh = new THREE.Mesh(geom, mat);
  mesh.position.set(cx, cy, GROUND_PLANE_Z_OFFSET);
  mesh.name = 'deshadowed-ortho';

  ctx.render3D({
    kind: 'mesh',
    object: mesh,
    layer: { id: 'ground-ortho', label: 'Ground (deshadowed ortho)', defaultVisible: true },
  });

  ctx.onCleanup(() => {
    geom.dispose();
    mat.dispose();
    texture.dispose();
  });
}

function clampU8(v) {
  if (Number.isNaN(v)) return 0;
  const n = Math.round(v * 255);
  return n < 0 ? 0 : n > 255 ? 255 : n;
}
