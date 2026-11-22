// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Generic helper: render a per-point RGB field as a 3D point cloud.
// Three input shapes via `inputType`:
//   - 'uint8'         — payload bytes are packed (R,G,B,R,G,B,...) ∈ [0,255]
//   - 'normalized'    — F32 packed (r,g,b,r,g,b,...) ∈ [0,1]
//   - 'abs-vec3-f32'  — F32 packed (x,y,z,x,y,z,...); colour = (|x|,|y|,|z|)
//                       clamped to [0,1] (this is what compute-normals does
//                       for lidar-normals — colour normals by |nx|,|ny|,|nz|).
//
// Shape discrimination across AK_POINT_COLUMN vs AK_ARRAY_INLINE is identical
// to scalar-gradient-cloud.js — see that file for the common pattern.
//
// `componentLabels` (default ['R','G','B']) controls the legend swatch
// labels — pass meaningful names like ['|nx|','|ny|','|nz|'] for normals
// or ['linearity','planarity','scattering'] for eigen-ternary.

import * as THREE from 'three';
import { parseBuildingHeader } from './per-building-records.js';

const AK_POINT_COLUMN = 10;
const AK_ARRAY_INLINE = 8;

export function renderPerPointRgbCloud(ctx, payload, config) {
  const cfg = {
    inputType: 'uint8',          // 'uint8' | 'normalized' | 'abs-vec3-f32'
    layerPath: undefined,
    layerLabel: payload?.name ?? 'rgb',
    layerId: payload?.name ?? 'rgb-cloud',
    pointSize: 0.1,
    componentLabels: ['R', 'G', 'B'],
    ...config,
  };

  const positionsArt = ctx.getSiblingDecoded?.('lidar-positions');
  if (!positionsArt || positionsArt.kind !== AK_POINT_COLUMN || !positionsArt.data) {
    ctx.renderHTML(
      `<div class="render-placeholder">${cfg.layerLabel}: subscribe lidar-positions to render the cloud.</div>`,
    );
    return;
  }
  const xyz = positionsArt.data;

  const collected = collectFromPayload(ctx, payload, xyz, cfg.inputType);
  if (!collected || collected.n === 0) {
    ctx.renderJSON({ artifact: payload?.name, n: 0, note: 'no points' });
    return;
  }
  const { positions, colors, n } = collected;

  let origin = ctx.viewer.getSceneOrigin?.();
  if (!origin) {
    origin = computeOrigin(positions, Math.min(n, 1000));
    ctx.viewer.setSceneOrigin?.(origin);
  }
  const centred = new Float32Array(n * 3);
  for (let i = 0; i < n; i++) {
    centred[i * 3 + 0] = positions[i * 3 + 0] - origin.x;
    centred[i * 3 + 1] = positions[i * 3 + 1] - origin.y;
    centred[i * 3 + 2] = positions[i * 3 + 2] - origin.z;
  }

  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.BufferAttribute(centred, 3));
  geom.setAttribute('color',    new THREE.BufferAttribute(colors,  3));
  const mat = new THREE.PointsMaterial({
    size: cfg.pointSize,
    sizeAttenuation: true,
    vertexColors: true,
  });
  const points = new THREE.Points(geom, mat);
  points.name = cfg.layerId;

  const layerSpec = {
    id: cfg.layerId,
    label: cfg.layerLabel,
    defaultVisible: false,
  };
  if (cfg.layerPath && cfg.layerPath.length) layerSpec.path = cfg.layerPath;

  ctx.render3D({ kind: 'points', object: points, layer: layerSpec });
  ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });

  // Legend: one swatch per RGB channel with the caller-supplied
  // semantic label (e.g. |nx|/|ny|/|nz| for normals).
  const labels = cfg.componentLabels;
  ctx.renderLegend?.({
    title: cfg.layerLabel,
    swatches: [
      { label: `${labels[0]} (R)`, color: 'rgb(220,55,55)' },
      { label: `${labels[1]} (G)`, color: 'rgb(55,200,80)' },
      { label: `${labels[2]} (B)`, color: 'rgb(70,110,230)' },
    ],
    note: cfg.inputType === 'abs-vec3-f32'
      ? 'colour = (|x|,|y|,|z|) clamped'
      : (cfg.inputType === 'uint8' ? 'uint8 RGB' : 'normalized RGB'),
  });

  ctx.renderJSON({
    artifact: payload?.name,
    n,
    inputType: cfg.inputType,
    layer: cfg.layerId,
  });
}

// ── Shape adapters ────────────────────────────────────────────────────

function collectFromPayload(ctx, payload, xyz, inputType) {
  if (!payload) return null;
  const kind = payload.kind;
  if (kind === AK_POINT_COLUMN) {
    return collectFromPointColumn(payload, xyz, inputType);
  }
  if (kind === AK_ARRAY_INLINE) {
    return collectFromArrayInline(ctx, payload, xyz, inputType);
  }
  return null;
}

function decodeRgbTriple(srcArr, srcIdx, inputType, out, outIdx) {
  if (inputType === 'uint8') {
    out[outIdx + 0] = srcArr[srcIdx + 0] / 255;
    out[outIdx + 1] = srcArr[srcIdx + 1] / 255;
    out[outIdx + 2] = srcArr[srcIdx + 2] / 255;
  } else if (inputType === 'normalized') {
    out[outIdx + 0] = clamp01(srcArr[srcIdx + 0]);
    out[outIdx + 1] = clamp01(srcArr[srcIdx + 1]);
    out[outIdx + 2] = clamp01(srcArr[srcIdx + 2]);
  } else {
    // abs-vec3-f32
    out[outIdx + 0] = clamp01(Math.abs(srcArr[srcIdx + 0]));
    out[outIdx + 1] = clamp01(Math.abs(srcArr[srcIdx + 1]));
    out[outIdx + 2] = clamp01(Math.abs(srcArr[srcIdx + 2]));
  }
}

function collectFromPointColumn(payload, xyz, inputType) {
  const raw = payload.data;
  // n_items in payload is n triples; data length = 3 * n_items.
  const n = Math.min(payload.n_items ?? (raw.length / 3) | 0, xyz.length / 3 | 0);
  const positions = new Float64Array(n * 3);
  const colors = new Float32Array(n * 3);
  for (let i = 0; i < n; i++) {
    positions[i * 3 + 0] = xyz[i * 3 + 0];
    positions[i * 3 + 1] = xyz[i * 3 + 1];
    positions[i * 3 + 2] = xyz[i * 3 + 2];
    decodeRgbTriple(raw, i * 3, inputType, colors, i * 3);
  }
  return { positions, colors, n };
}

function collectFromArrayInline(ctx, payload, xyz, inputType) {
  const items = payload.items || [];
  const buildings = ctx.getSiblingDecoded?.('per-building-records');
  if (!buildings?.items) {
    ctx.renderHTML(
      `<div class="render-placeholder">${payload.name}: subscribe per-building-records to map records to points.</div>`,
    );
    return null;
  }
  const isF32 = inputType !== 'uint8';
  const elemSize = isF32 ? 4 : 1;
  // Each per-building record holds n_pts × 3 RGB triples → 3*n_pts elements
  // → 3*n_pts*elemSize bytes.
  let total = 0;
  for (let bi = 0; bi < items.length; bi++) {
    const u8 = items[bi];
    if (!u8 || u8.byteLength === 0) continue;
    total += (u8.byteLength / (elemSize * 3)) | 0;
  }
  if (total === 0) return { positions: new Float64Array(0), colors: new Float32Array(0), n: 0 };

  const positions = new Float64Array(total * 3);
  const colors = new Float32Array(total * 3);
  let cursor = 0;
  for (let bi = 0; bi < items.length; bi++) {
    const u8 = items[bi];
    if (!u8 || u8.byteLength === 0) continue;
    const tripleCount = (u8.byteLength / (elemSize * 3)) | 0;
    const view = isF32
      ? new Float32Array(u8.buffer, u8.byteOffset, tripleCount * 3)
      : new Uint8Array(u8.buffer, u8.byteOffset, tripleCount * 3);
    const bldgU8 = buildings.items[bi];
    const hdr = parseBuildingHeader(bldgU8);
    if (!hdr || hdr.n_pts !== tripleCount) continue;
    const gidx = hdr.globalIndices;
    for (let i = 0; i < hdr.n_pts; i++) {
      const g = gidx[i];
      positions[cursor * 3 + 0] = xyz[g * 3 + 0];
      positions[cursor * 3 + 1] = xyz[g * 3 + 1];
      positions[cursor * 3 + 2] = xyz[g * 3 + 2];
      decodeRgbTriple(view, i * 3, inputType, colors, cursor * 3);
      cursor++;
    }
  }
  return {
    positions: positions.subarray(0, cursor * 3),
    colors: colors.subarray(0, cursor * 3),
    n: cursor,
  };
}

function computeOrigin(positions, sample) {
  let sx = 0, sy = 0, sz = 0;
  const n = sample;
  for (let i = 0; i < n; i++) {
    sx += positions[i * 3 + 0];
    sy += positions[i * 3 + 1];
    sz += positions[i * 3 + 2];
  }
  return { x: sx / n, y: sy / n, z: sz / n };
}

function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
