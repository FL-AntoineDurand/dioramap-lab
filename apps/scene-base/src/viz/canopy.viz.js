// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand

const t = (s) => s;

// canopy.viz.js — leaf-textured hex billboards for tree/bush points.
//
// Reads:
//   lidar-positions               AK_POINT_COLUMN F64×3 — L93 metres
//   lidar-normals                 AK_POINT_COLUMN F32×3 — unit normals (Z-up)
//   lidar-spacing                 AK_POINT_COLUMN F32×1 — per-point local spacing
//   canopy-flags                  AK_POINT_COLUMN U8×1 — 1 = canopy point
//   deshadowed-ortho-rgb-rasters  AK_RASTER_F32 ≥3 ch — for the per-vertex tint
//   canonical-location            OPAQUE_JSON {bbox_l93}
//   scene-origin                  OPAQUE_JSON {x, y, z}
//
// Output: one InstancedMesh of CircleGeometry(1, 6) hex discs, one or two
// instances per canopy point, scaled by spacing, oriented to the local
// surface normal with a small random tilt for canopy depth. Leaf texture
// from generateLeafTexture (S3 utility) discards via alphaTest in the
// fragment shader so the silhouettes pop. Cast shadows use a separate
// solid-hex depth material at 25% scale.
//
// Ported from lidar-3d-environment-viewer/frontend/src/point-cloud/canopy-renderer.js,
// converted to v3 Z-up convention (legacy was Y-up — every (x,y,z) ↔ y/z
// swap and `_zAxis` rotation source removed).

import * as THREE from 'three';
import { generateLeafTexture } from '../utilities/leaf-sprite.js';

export const meta = {
  name: 'Canopy (leaf-textured discs)',
  requires: [
    'lidar-positions',
    'lidar-normals',
    'lidar-spacing',
    'canopy-flags',
    'deshadowed-ortho-rgb-rasters',
    'canonical-location',
    'scene-origin',
  ],
  pipelines: '*',
  default: true,
};

const MAX_TILT = 20 * Math.PI / 180;
const SHADOW_SIZE_FACTOR = 0.25;

// Texture cache — generating the procedural leaf mosaic is deterministic
// and identical across composite invocations on this page. One cached
// texture serves every canopy render.
let _leafTextureCache = null;
function getLeafTexture() {
  if (_leafTextureCache) return _leafTextureCache;
  _leafTextureCache = generateLeafTexture();
  return _leafTextureCache;
}

export function render(ctx, decoded) {
  const pos = decoded['lidar-positions'];
  const norm = decoded['lidar-normals'];
  const spc = decoded['lidar-spacing'];
  const can = decoded['canopy-flags'];
  const ortho = decoded['deshadowed-ortho-rgb-rasters'];
  const loc = decoded['canonical-location'];
  const origin = decoded['scene-origin']?.json;
  if (!pos?.data || !norm?.data || !spc?.data || !can?.data) return;
  if (!ortho?.channels || ortho.channels.length < 3) return;
  if (!loc?.json?.bbox_l93 || !origin) return;

  const xyz = pos.data;
  const normals = norm.data;
  const spacing = spc.data;
  const canopyFlags = can.data;
  const n = pos.n_items;

  // Filter to canopy-flagged points only and recenter via scene-origin.
  const idx = [];
  for (let i = 0; i < n; i++) if (canopyFlags[i]) idx.push(i);
  if (idx.length === 0) return;

  const cnt = idx.length;
  const positions = new Float32Array(cnt * 3);
  const cnormals  = new Float32Array(cnt * 3);
  const cspacing  = new Float32Array(cnt);
  for (let k = 0; k < cnt; k++) {
    const i = idx[k];
    positions[k * 3 + 0] = xyz[i * 3 + 0] - origin.x;
    positions[k * 3 + 1] = xyz[i * 3 + 1] - origin.y;
    positions[k * 3 + 2] = xyz[i * 3 + 2] - origin.z;
    cnormals[k * 3 + 0] = normals[i * 3 + 0];
    cnormals[k * 3 + 1] = normals[i * 3 + 1];
    cnormals[k * 3 + 2] = normals[i * 3 + 2];
    cspacing[k] = spacing[i];
  }

  // Build the ortho texture for the per-vertex tint shader.
  const orthoTex = buildRGBTexture(ortho);

  const [xmin, ymin, xmax, ymax] = loc.json.bbox_l93;
  const sceneXmin = xmin - origin.x;
  const sceneYmin = ymin - origin.y;
  const sceneSizeX = xmax - xmin;
  const sceneSizeY = ymax - ymin;

  // ── Geometry + material ────────────────────────────────────────
  const geom = new THREE.CircleGeometry(1, 6);

  const material = new THREE.MeshStandardMaterial({
    roughness: 0.85,
    metalness: 0.0,
    side: THREE.DoubleSide,
    transparent: false,
    depthWrite: true,
  });

  const leafTex = getLeafTexture();
  material.onBeforeCompile = (shader) => {
    shader.uniforms.uOrthoTex = { value: orthoTex };
    shader.uniforms.uOrthoMinScene = { value: new THREE.Vector2(sceneXmin, sceneYmin) };
    shader.uniforms.uOrthoSizeScene = { value: new THREE.Vector2(sceneSizeX, sceneSizeY) };
    shader.uniforms.uLeafTex = { value: leafTex };
    material.userData.shader = shader;

    // Z-up: world XY plane is the ground; (worldX, worldY) → ortho UV.
    shader.vertexShader = shader.vertexShader
      .replace('#include <common>',
        `#include <common>
         varying vec3 vOrthoTint;
         varying vec2 vLeafUV;
         uniform sampler2D uOrthoTex;
         uniform vec2 uOrthoMinScene;
         uniform vec2 uOrthoSizeScene;`)
      .replace('#include <project_vertex>',
        `#include <project_vertex>
         vec4 wp = modelMatrix * instanceMatrix * vec4(position, 1.0);
         float uu = (wp.x - uOrthoMinScene.x) / uOrthoSizeScene.x;
         float vv = (wp.y - uOrthoMinScene.y) / uOrthoSizeScene.y;
         vOrthoTint = texture2D(uOrthoTex, vec2(uu, vv)).rgb;
         vLeafUV = uv;`);

    shader.fragmentShader = shader.fragmentShader
      .replace('#include <common>',
        `#include <common>
         varying vec3 vOrthoTint;
         varying vec2 vLeafUV;
         uniform sampler2D uLeafTex;`)
      .replace('vec4 diffuseColor = vec4( diffuse, opacity );',
        `vec4 texSample = texture2D(uLeafTex, vLeafUV);
         if (texSample.a < 0.3) discard;
         vec3 color = mix(vOrthoTint, texSample.rgb, 0.1);
         float edgeDark = smoothstep(0.3, 0.65, texSample.a);
         color *= mix(0.35, 1.0, edgeDark);
         vec4 diffuseColor = vec4(color, 1.0);`);
  };

  // Shadow depth material — solid hex at 25% scale. No leaf alpha, no
  // DoubleSide (back-faces would double-write depth and produce artefacts
  // on tilted instances).
  const depthMaterial = new THREE.MeshDepthMaterial({
    depthPacking: THREE.RGBADepthPacking,
  });
  depthMaterial.onBeforeCompile = (shader) => {
    shader.vertexShader = shader.vertexShader.replace(
      '#include <begin_vertex>',
      `vec3 transformed = vec3(position) * ${SHADOW_SIZE_FACTOR.toFixed(4)};`,
    );
  };

  // ── Per-instance transforms ────────────────────────────────────
  // Two instances per canopy point — one upright on the local normal,
  // one with random tilt. Z-up: CircleGeometry(1, 6) lies in XY with
  // normal +Z by default, so setFromUnitVectors(+Z, surfaceNormal)
  // orients the disc to the surface.
  const INSTANCES_PER_POINT = 2;
  const totalInstances = cnt * INSTANCES_PER_POINT;
  const mesh = new THREE.InstancedMesh(geom, material, totalInstances);
  mesh.castShadow = true;
  mesh.receiveShadow = true;
  mesh.name = 'canopy';
  mesh.instanceMatrix.setUsage(THREE.StaticDrawUsage);
  mesh.customDepthMaterial = depthMaterial;

  const dummy = new THREE.Object3D();
  const zAxis = new THREE.Vector3(0, 0, 1);
  const normal = new THREE.Vector3();
  const axis = new THREE.Vector3();
  const tilt = new THREE.Quaternion();

  // Deterministic LCG.
  let seed = 54321;
  const rand = () => {
    seed = (seed * 16807) % 2147483647;
    return (seed - 1) / 2147483646;
  };

  const sizeFactor = 0.7;
  const sizeBloat = 1.3;
  const minRadius = 0.05;
  const maxRadius = 1.2;

  let instIdx = 0;
  for (let i = 0; i < cnt; i++) {
    const o = i * 3;
    const px = positions[o], py = positions[o + 1], pz = positions[o + 2];
    normal.set(cnormals[o], cnormals[o + 1], cnormals[o + 2]);
    if (normal.lengthSq() < 0.5) normal.set(0, 0, 1);

    let r = cspacing[i] * sizeFactor * sizeBloat;
    if (r < minRadius) r = minRadius;
    else if (r > maxRadius) r = maxRadius;

    for (let j = 0; j < INSTANCES_PER_POINT; j++) {
      dummy.position.set(px, py, pz);
      if (j === 0) {
        dummy.quaternion.setFromUnitVectors(zAxis, normal);
      } else {
        const tiltAngle = (rand() - 0.5) * 2 * MAX_TILT;
        const rollAngle = rand() * Math.PI * 2;
        // Z-up: tilt axis is in the XY plane perpendicular to up.
        axis.set(Math.cos(rollAngle), Math.sin(rollAngle), 0);
        tilt.setFromAxisAngle(axis, tiltAngle);
        dummy.quaternion.setFromUnitVectors(zAxis, normal);
        dummy.quaternion.multiply(tilt);
        dummy.position.x += (rand() - 0.5) * r * 0.4;
        dummy.position.y += (rand() - 0.5) * r * 0.4;
        dummy.position.z += (rand() - 0.5) * r * 0.2;
      }
      const scale = r * (0.85 + rand() * 0.3);
      dummy.scale.set(scale, scale, 1);
      dummy.updateMatrix();
      mesh.setMatrixAt(instIdx++, dummy.matrix);
    }
  }
  mesh.instanceMatrix.needsUpdate = true;

  ctx.render3D({
    kind: 'instanced-mesh',
    object: mesh,
    // Group with the disc composites under one 'Point cloud' parent
    // node so the visibility tree keeps the three per-class outputs
    // (ground / other / canopy) co-located under one collapsible group.
    layer: { id: 'canopy', label: 'Canopy', path: ['Point cloud'], defaultVisible: true },
  });

  ctx.onCleanup(() => {
    geom.dispose();
    material.dispose();
    depthMaterial.dispose();
    orthoTex.dispose();
  });
}

// Build a CanvasTexture from the F32 RGB raster's first 3 channels.
// Matches the deshadowed-ortho viz's clamp + sRGB convention.
function buildRGBTexture(ortho) {
  const { cols, rows, channels } = ortho;
  const r = channels[0], g = channels[1], b = channels[2];
  const canvas = document.createElement('canvas');
  canvas.width = cols;
  canvas.height = rows;
  const c2d = canvas.getContext('2d');
  const img = c2d.createImageData(cols, rows);
  for (let py = 0; py < rows; py++) {
    const srcRow = rows - 1 - py;
    for (let px = 0; px < cols; px++) {
      const s = srcRow * cols + px;
      const di = (py * cols + px) * 4;
      img.data[di + 0] = clampU8(r[s]);
      img.data[di + 1] = clampU8(g[s]);
      img.data[di + 2] = clampU8(b[s]);
      img.data[di + 3] = 255;
    }
  }
  c2d.putImageData(img, 0, 0);
  const tex = new THREE.CanvasTexture(canvas);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.minFilter = THREE.LinearFilter;
  tex.magFilter = THREE.LinearFilter;
  tex.needsUpdate = true;
  return tex;
}

function clampU8(v) {
  if (Number.isNaN(v)) return 0;
  const n = Math.round(v * 255);
  return n < 0 ? 0 : n > 255 ? 255 : n;
}
