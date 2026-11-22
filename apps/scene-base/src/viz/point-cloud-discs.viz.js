// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand

const t = (s) => s;

// point-cloud-discs.viz.js — oriented-disc point cloud (ground + other split).
//
// Reads:
//   lidar-positions               AK_POINT_COLUMN F64×3 — L93 metres
//   lidar-normals                 AK_POINT_COLUMN F32×3 — unit normals (Z-up)
//   lidar-spacing                 AK_POINT_COLUMN F32×1 — per-point local spacing
//   lidar-classifications         AK_POINT_COLUMN U8×1  — ASPRS class
//   canopy-flags                  AK_POINT_COLUMN U8×1  — 1 = canopy (skipped here; rendered by canopy.viz)
//   grass-flags                   AK_POINT_COLUMN U8×1  — 1 = grass (only meaningful on ground points)
//   deshadowed-ortho-rgb-rasters  AK_RASTER_F32 ≥3 ch
//   canonical-location            OPAQUE_JSON {bbox_l93}
//   scene-origin                  OPAQUE_JSON {x, y, z}
//
// Two THREE.InstancedMesh outputs per the legacy SDK's split:
//   GROUND (ASPRS class 2, non-canopy):
//     - subdivided hex DOME geometry; vertex shader pushes Z up via a
//       parabolic profile (uDomeH) plus per-instance hash noise (uNoiseAmp)
//     - flat normals on the dome (objectNormal = +Z) so Lambert is uniform
//       across the dome
//     - fragment writes RGB = diffuseColor × getShadowMask(): the RGB
//       stomp to pre-lighting diffuseColor bypasses three.js's view-
//       dependent lighting (env IBL + Fresnel) which produces subtle
//       gradients across the curved dome that flat-normal alone doesn't
//       eliminate. Multiply by getShadowMask() so the dome still
//       receives cast shadows from buildings / trees / etc. in the
//       scene. (EDL is OFF for ground discs — default behaviour:
//       ground never calls setEdlOptIn so its mesh isn't in the EDL
//       layer mask and the EDL pass leaves its pixels untouched.)
//     - per-instance grass tint via instanceColor (green boost on flagged
//       points; neutral on non-grass)
//     - cast/receive shadows
//   OTHER (everything non-ground, non-canopy):
//     - flat CircleGeometry(1, 6) hex
//     - opts INTO EDL via setEdlOptIn(mesh) — enables EDL_LAYER on
//       its layers so the mask-render pass marks its pixels and the
//       EDL post-process applies darkening
//     - cast/receive shadows
//
// Both share: per-vertex ortho sampling (worldXY → UV; Z-up), radial
// alpha fade in the outer 40% of the hex for soft silhouettes, and a
// 25%-scale solid-hex shadow depth material so cast shadows match the
// underlying physical lidar point footprint rather than the visually
// inflated visible hex.
//
// Ported from lidar-3d-environment-viewer/frontend/src/point-cloud/
// disc-renderer.js — converted to v3 Z-up convention.

import * as THREE from 'three';
import { setEdlOptIn } from '../scene/create-scene.js';

export const meta = {
  name: 'Point cloud (oriented hex discs)',
  requires: [
    'lidar-positions',
    'lidar-normals',
    'lidar-spacing',
    'lidar-classifications',
    'canopy-flags',
    'grass-flags',
    'deshadowed-ortho-rgb-rasters',
    'canonical-location',
    'scene-origin',
  ],
  pipelines: '*',
  default: true,
  // Hides the streaming preview composite once the high-quality discs
  // have rendered. Both SDK orchestration and lab-ui dispatcher honour
  // this on first successful render.
  supersedes: ['lidar-positions-preview'],
};

const SHADOW_SIZE_FACTOR = 0.25;
const SIZE_FACTOR = 0.75;
const SIZE_BLOAT = 1.5;
const MIN_RADIUS = 0.05;
const MAX_RADIUS = 1.5;
const DOME_HEIGHT = 0.6;
const DOME_NOISE = 0.15;

export function render(ctx, decoded) {
  const pos = decoded['lidar-positions'];
  const norm = decoded['lidar-normals'];
  const spc = decoded['lidar-spacing'];
  const cls = decoded['lidar-classifications'];
  const can = decoded['canopy-flags'];
  const grs = decoded['grass-flags'];
  const ortho = decoded['deshadowed-ortho-rgb-rasters'];
  const loc = decoded['canonical-location'];
  const origin = decoded['scene-origin']?.json;
  if (!pos?.data || !norm?.data || !spc?.data || !cls?.data || !can?.data || !grs?.data) return;
  if (!ortho?.channels || ortho.channels.length < 3) return;
  if (!loc?.json?.bbox_l93 || !origin) return;

  const xyz = pos.data;
  const normals = norm.data;
  const spacing = spc.data;
  const classes = cls.data;
  const canopy = can.data;
  const grass = grs.data;
  const n = pos.n_items;

  // Three-way split: skip canopy; remaining → ground (ASPRS 2) + other.
  const groundIdx = [];
  const otherIdx = [];
  for (let i = 0; i < n; i++) {
    if (canopy[i]) continue;
    if (classes[i] === 2) groundIdx.push(i);
    else otherIdx.push(i);
  }
  if (groundIdx.length === 0 && otherIdx.length === 0) return;

  // Build the shared ortho texture once.
  const orthoTex = buildRGBTexture(ortho);

  const [xmin, ymin, xmax, ymax] = loc.json.bbox_l93;
  const sceneXmin = xmin - origin.x;
  const sceneYmin = ymin - origin.y;
  const sceneSizeX = xmax - xmin;
  const sceneSizeY = ymax - ymin;
  const orthoUniforms = {
    uOrthoTex: { value: orthoTex },
    uOrthoMinScene: { value: new THREE.Vector2(sceneXmin, sceneYmin) },
    uOrthoSizeScene: { value: new THREE.Vector2(sceneSizeX, sceneSizeY) },
  };

  if (groundIdx.length > 0) {
    const mesh = buildBatch({
      indices: groundIdx,
      xyz, normals, spacing, grass,
      origin,
      orthoUniforms,
      dome: true,
      withGrassTint: true,
      name: 'lidar-discs-ground',
    });
    // No setEdlOptIn — ground discs render flat-lit, EDL stays off.
    ctx.render3D({
      kind: 'instanced-mesh',
      object: mesh,
      layer: { id: 'discs-ground', label: 'Discs (ground)', path: ['Point cloud'], defaultVisible: true },
    });
    ctx.onCleanup(() => { mesh.geometry.dispose(); mesh.material.dispose(); mesh.customDepthMaterial?.dispose(); });
  }

  if (otherIdx.length > 0) {
    const mesh = buildBatch({
      indices: otherIdx,
      xyz, normals, spacing, grass,
      origin,
      orthoUniforms,
      dome: false,
      withGrassTint: false,
      name: 'lidar-discs-other',
    });
    // Opt INTO EDL: enables the EDL_LAYER bit on this mesh so the
    // post-process applies the silhouette-darkening halo that gives
    // non-ground points their 3D feel.
    setEdlOptIn(mesh);
    ctx.render3D({
      kind: 'instanced-mesh',
      object: mesh,
      layer: { id: 'discs-other', label: 'Discs (other)', path: ['Point cloud'], defaultVisible: true },
    });
    ctx.onCleanup(() => { mesh.geometry.dispose(); mesh.material.dispose(); mesh.customDepthMaterial?.dispose(); });
  }

  ctx.onCleanup(() => orthoTex.dispose());
}

// One InstancedMesh — geometry/material flavour selected by `dome`.
// Per-instance position/quat/scale are computed from the lidar point's
// xyz/normal/spacing. EDL opt-in is the caller's responsibility (call
// setEdlOptIn(mesh) after buildBatch returns).
function buildBatch({
  indices, xyz, normals, spacing, grass, origin,
  orthoUniforms, dome, withGrassTint, name,
}) {
  const cnt = indices.length;
  const geom = dome ? makeHexDomeGeometry(3) : new THREE.CircleGeometry(1, 6);

  const material = new THREE.MeshStandardMaterial({
    roughness: 0.9,
    metalness: 0.0,
    side: THREE.DoubleSide,
    // Dome ground discs render opaque (the RGB-stomp shader chunk
    // writes a final colour and we want full ground coverage, no
    // hex-perimeter fade). Non-dome discs are transparent with an
    // alpha-test cutoff for the soft hex outline.
    transparent: !dome,
    alphaTest: 0.05,
    depthWrite: true,
  });

  material.onBeforeCompile = (shader) => {
    Object.assign(shader.uniforms, orthoUniforms);
    if (dome) {
      shader.uniforms.uDomeH = { value: DOME_HEIGHT };
      shader.uniforms.uNoiseAmp = { value: DOME_NOISE };
    }

    // VERTEX — per-vertex ortho sample (worldXY → UV; Z-up). When
    // dome=true, also push Z up via parabolic profile + per-instance
    // hash noise (instance origin seeds the noise so each disc has a
    // unique bump pattern). Use the UNDISPLACED position attribute for
    // the ortho UV so dome vertices still sample the ground footprint
    // directly below, not at apex altitude.
    let common = `
      #include <common>
      varying vec3 vOrthoSampled;
      varying float vHexR;
      uniform sampler2D uOrthoTex;
      uniform vec2 uOrthoMinScene;
      uniform vec2 uOrthoSizeScene;
    `;
    if (dome) {
      common += `
      uniform float uDomeH;
      uniform float uNoiseAmp;
      float _hash(vec2 p) {
        return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
      }
      `;
    }
    shader.vertexShader = shader.vertexShader.replace('#include <common>', common);

    shader.vertexShader = shader.vertexShader.replace(
      '#include <project_vertex>',
      `#include <project_vertex>
       vec4 wp = modelMatrix * instanceMatrix * vec4(position, 1.0);
       float uu = (wp.x - uOrthoMinScene.x) / uOrthoSizeScene.x;
       float vv = (wp.y - uOrthoMinScene.y) / uOrthoSizeScene.y;
       vOrthoSampled = texture2D(uOrthoTex, vec2(uu, vv)).rgb;
       vHexR = length(position.xy);`
    );

    if (dome) {
      // Dome Z displacement in local space, before instanceMatrix so it
      // rotates with the disc's normal orientation. Instance scale is
      // uniform (r,r,r) so the dome height scales with disc size in
      // world units.
      shader.vertexShader = shader.vertexShader.replace(
        '#include <begin_vertex>',
        `vec3 transformed = vec3( position );
         float _r2 = dot(position.xy, position.xy);
         float _domeZ = uDomeH * (1.0 - _r2);
         vec3 _instO = instanceMatrix[3].xyz;
         float _n = _hash(position.xy * 4.0 + _instO.xy * 17.31) - 0.5;
         transformed.z += _domeZ + _n * uNoiseAmp;`
      );
      // Flat normal across the dome so the apex doesn't darken under
      // low sun. Dome is for silhouette/volume, not curved shading.
      shader.vertexShader = shader.vertexShader.replace(
        '#include <beginnormal_vertex>',
        `vec3 objectNormal = vec3( 0.0, 0.0, 1.0 );
         #ifdef USE_TANGENT
           vec3 objectTangent = vec3( tangent.xyz );
         #endif`
      );
    }

    // FRAGMENT — substitute material color with per-vertex ortho
    // sample, apply soft radial alpha fade. We also pull in
    // <shadowmask_pars_fragment> for the dome's manual shadow modulation:
    // MeshStandardMaterial doesn't include it by default (only lambert-
    // style materials do). It must be injected AFTER
    // <shadowmap_pars_fragment>, where the DirectionalLightShadow struct
    // and `directionalLightShadows[]` uniform array are declared. Anchor
    // on <clipping_planes_pars_fragment>: it's the last *_pars_fragment
    // include in meshphysical.glsl before void main(), so by then both
    // the lights and the shadow-map declarations are in scope.
    shader.fragmentShader = shader.fragmentShader
      .replace('#include <common>',
        `#include <common>
         varying vec3 vOrthoSampled;
         varying float vHexR;`)
      .replace('#include <clipping_planes_pars_fragment>',
        `#include <clipping_planes_pars_fragment>
         #include <shadowmask_pars_fragment>`)
      .replace('vec4 diffuseColor = vec4( diffuse, opacity );',
        `vec4 diffuseColor = vec4(vOrthoSampled, opacity);
         float hexAlpha = 1.0 - smoothstep(0.6, 1.0, vHexR);
         diffuseColor.a *= hexAlpha;`);

    if (dome) {
      // DOME mode RGB stomp at <opaque_fragment>: replace the lit
      // outgoingLight with the pre-lighting diffuseColor (ortho ×
      // instance tint), multiplied by getShadowMask() so shadows are
      // still received. Three's standard lighting pipeline produces
      // view-dependent variation across the curved dome (env IBL +
      // Fresnel + tonemapping interactions) and flat-normal alone
      // doesn't fully eliminate it — chasing the "dome tip is black"
      // artefact cost a long debugging pass in the legacy v2 SDK
      // (commit 801aae5). The RGB stomp bypasses all of it. Lambert
      // n·L is intentionally dropped: the legacy always rendered the
      // dome at full albedo regardless of sun angle, leaving the
      // surrounding non-dome discs (which DO take Lambert) to carry
      // the lighting cue.
      shader.fragmentShader = shader.fragmentShader.replace(
        '#include <opaque_fragment>',
        `#include <opaque_fragment>
         gl_FragColor = vec4(diffuseColor.rgb * getShadowMask(), 1.0);`
      );
    }
  };

  // Tighter cast shadows: 25%-scale solid hex in the depth pass.
  // Reads the unmodified `position` attribute so the dome's vertex
  // displacement doesn't bleed into the shadow silhouette.
  const depthMaterial = new THREE.MeshDepthMaterial({
    depthPacking: THREE.RGBADepthPacking,
  });
  depthMaterial.onBeforeCompile = (shader) => {
    shader.vertexShader = shader.vertexShader.replace(
      '#include <begin_vertex>',
      `vec3 transformed = vec3(position) * ${SHADOW_SIZE_FACTOR.toFixed(4)};`,
    );
  };

  const mesh = new THREE.InstancedMesh(geom, material, cnt);
  mesh.castShadow = true;
  mesh.receiveShadow = true;
  mesh.name = name;
  mesh.instanceMatrix.setUsage(THREE.StaticDrawUsage);
  mesh.customDepthMaterial = depthMaterial;

  // Per-instance grass tint: ground points flagged grass get a green
  // boost (0.80, 1.35, 0.70). Three's `#include <color_fragment>` in
  // the standard material auto-multiplies diffuseColor by vColor (from
  // instanceColor) — no shader changes needed beyond enabling it.
  if (withGrassTint) {
    const colors = new Float32Array(cnt * 3);
    for (let k = 0; k < cnt; k++) {
      const i = indices[k];
      if (grass[i]) { colors[k * 3] = 0.80; colors[k * 3 + 1] = 1.35; colors[k * 3 + 2] = 0.70; }
      else          { colors[k * 3] = 1.0;  colors[k * 3 + 1] = 1.0;  colors[k * 3 + 2] = 1.0; }
    }
    mesh.instanceColor = new THREE.InstancedBufferAttribute(colors, 3);
    mesh.instanceColor.needsUpdate = true;
  }

  // Per-instance transforms. Z-up: CircleGeometry(1, 6) lies in XY
  // with normal +Z, so setFromUnitVectors(+Z, surfaceNormal) orients
  // the disc to the local surface. Dome geometry shares the same
  // base orientation; the parabolic Z rise is added in the shader.
  const dummy = new THREE.Object3D();
  const zAxis = new THREE.Vector3(0, 0, 1);
  const normal = new THREE.Vector3();

  for (let k = 0; k < cnt; k++) {
    const i = indices[k];
    const o = i * 3;
    dummy.position.set(xyz[o] - origin.x, xyz[o + 1] - origin.y, xyz[o + 2] - origin.z);
    normal.set(normals[o], normals[o + 1], normals[o + 2]);
    if (normal.lengthSq() < 0.5) normal.set(0, 0, 1);
    dummy.quaternion.setFromUnitVectors(zAxis, normal);

    let r = spacing[i] * SIZE_FACTOR * SIZE_BLOAT;
    if (r < MIN_RADIUS) r = MIN_RADIUS;
    else if (r > MAX_RADIUS) r = MAX_RADIUS;
    // Uniform scale in dome mode so the vertex-shader Z displacement
    // scales proportionally to the disc radius. Flat mode leaves Z
    // unscaled (nothing to scale on a flat disc).
    dummy.scale.set(r, r, dome ? r : 1);

    dummy.updateMatrix();
    mesh.setMatrixAt(k, dummy.matrix);
  }
  mesh.instanceMatrix.needsUpdate = true;

  // Manual bounding sphere from the recentered point cloud + max disc
  // radius. Three.js' default for InstancedMesh inflates conservatively
  // from unit-geometry × instanceMatrix.
  const box = new THREE.Box3();
  for (let k = 0; k < cnt; k++) {
    const i = indices[k];
    const o = i * 3;
    box.expandByPoint(new THREE.Vector3(
      xyz[o] - origin.x, xyz[o + 1] - origin.y, xyz[o + 2] - origin.z,
    ));
  }
  box.expandByScalar(MAX_RADIUS);
  mesh.boundingBox = box;
  mesh.boundingSphere = new THREE.Sphere();
  box.getBoundingSphere(mesh.boundingSphere);

  return mesh;
}

// Hexagonal disc with `rings` radial subdivisions for dome displacement.
// 6 angular segments matches CircleGeometry(1, 6) so the silhouette
// looks identical to the flat disc; extra interior vertices let the
// vertex shader smoothly raise Z into a dome.
function makeHexDomeGeometry(rings) {
  const segments = 6;
  const positions = [0, 0, 0];
  const uvs = [0.5, 0.5];
  const normals = [0, 0, 1];
  const indices = [];

  for (let ri = 1; ri <= rings; ri++) {
    const r = ri / rings;
    for (let si = 0; si < segments; si++) {
      const a = (si / segments) * Math.PI * 2;
      const x = Math.cos(a) * r;
      const y = Math.sin(a) * r;
      positions.push(x, y, 0);
      uvs.push((x + 1) * 0.5, (y + 1) * 0.5);
      normals.push(0, 0, 1);
    }
  }
  // Inner fan.
  for (let si = 0; si < segments; si++) {
    const a = 1 + si;
    const b = 1 + ((si + 1) % segments);
    indices.push(0, b, a);
  }
  // Outer ring quads.
  for (let ri = 0; ri < rings - 1; ri++) {
    const inner = 1 + ri * segments;
    const outer = inner + segments;
    for (let si = 0; si < segments; si++) {
      const a = inner + si;
      const b = inner + ((si + 1) % segments);
      const c = outer + si;
      const d = outer + ((si + 1) % segments);
      indices.push(a, c, b);
      indices.push(b, c, d);
    }
  }
  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
  geo.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
  geo.setAttribute('normal', new THREE.Float32BufferAttribute(normals, 3));
  geo.setIndex(indices);
  return geo;
}

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
