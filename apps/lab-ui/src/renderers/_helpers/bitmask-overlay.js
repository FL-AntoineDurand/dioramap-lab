// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Generic helper: render a Uint8 per-point bitmask as a 3D point cloud
// with distinct hue per active bit. Convention from bench-v2 ridge-
// candidates: bit 7 (0x80) = orphan, lower bits = ridge classes.
//
// Sibling reads:
//   - lidar-positions  (F64×3 LAZ positions)
//   - per-building-records  (when payload is AK_ARRAY_INLINE)
//
// Config knobs:
//   { layerPath, layerLabel, layerId, pointSize, bitColors,
//     unassignedColor }
// `bitColors` is an array of 8 THREE.Color-friendly values, one per bit
// position (0 = LSB). The first matching bit wins.

import * as THREE from 'three';
import { parseBuildingHeader } from './per-building-records.js';

const AK_POINT_COLUMN = 10;
const AK_ARRAY_INLINE = 8;

const DEFAULT_BIT_COLORS = [
  '#ff4040', // bit 0 — ridge class 0
  '#22cc44', // bit 1 — eave
  '#0099cc', // bit 2 — dormer
  '#ff8c1a', // bit 3
  '#a64dff', // bit 4
  '#00ffcc', // bit 5
  '#ffcc00', // bit 6 — gap
  '#cc55ff', // bit 7 — orphan (special)
];

export function renderBitmaskOverlay(ctx, payload, config) {
  const cfg = {
    layerPath: undefined,
    layerLabel: payload?.name ?? 'bitmask',
    layerId: payload?.name ?? 'bitmask-cloud',
    pointSize: 0.1,
    bitColors: DEFAULT_BIT_COLORS,
    inactiveColor: { r: 0.16, g: 0.18, b: 0.21 },
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

  const collected = collect(ctx, payload, xyz);
  if (!collected || collected.n === 0) {
    ctx.renderJSON({ artifact: payload?.name, n: 0, note: 'no points' });
    return;
  }
  const { positions, bytes, n } = collected;

  const tmp = new THREE.Color();
  const bitTHREE = cfg.bitColors.map((s) => new THREE.Color(s));
  let origin = ctx.viewer.getSceneOrigin?.();
  if (!origin) {
    origin = computeOrigin(positions, Math.min(n, 1000));
    ctx.viewer.setSceneOrigin?.(origin);
  }
  const centred = new Float32Array(n * 3);
  const colors = new Float32Array(n * 3);
  const bitCounts = new Uint32Array(8);
  let nActive = 0;
  for (let i = 0; i < n; i++) {
    centred[i * 3 + 0] = positions[i * 3 + 0] - origin.x;
    centred[i * 3 + 1] = positions[i * 3 + 1] - origin.y;
    centred[i * 3 + 2] = positions[i * 3 + 2] - origin.z;
    const v = bytes[i];
    if (v === 0) {
      colors[i * 3 + 0] = cfg.inactiveColor.r;
      colors[i * 3 + 1] = cfg.inactiveColor.g;
      colors[i * 3 + 2] = cfg.inactiveColor.b;
    } else {
      // Pick first set bit (bit 7 wins because it's "orphan" — most
      // informative on hemisphere-segment output).
      let pickedBit = 0;
      for (let b = 7; b >= 0; b--) {
        if (v & (1 << b)) { pickedBit = b; bitCounts[b]++; break; }
      }
      tmp.copy(bitTHREE[pickedBit]);
      colors[i * 3 + 0] = tmp.r;
      colors[i * 3 + 1] = tmp.g;
      colors[i * 3 + 2] = tmp.b;
      nActive++;
    }
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
  if (cfg.layerPath?.length) layerSpec.path = cfg.layerPath;

  ctx.render3D({ kind: 'points', object: points, layer: layerSpec });
  ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });

  // Legend: one row per active bit (with its count) + inactive total.
  // Bit-name labels are domain-specific; cfg.bitLabels (optional) lets
  // the caller override the default "bit N" labels — e.g. ridge-
  // candidates passes ['', '', '', '', '', '', '', 'orphan'].
  const swatches = [];
  for (let b = 0; b < 8; b++) {
    if (bitCounts[b] === 0) continue;
    const label = cfg.bitLabels?.[b] || `bit ${b}`;
    swatches.push({ label, color: cfg.bitColors[b], count: bitCounts[b] });
  }
  const nInactive = n - nActive;
  if (nInactive > 0) {
    const u = cfg.inactiveColor;
    swatches.push({
      label: 'inactive (no bit)',
      color: `rgb(${(u.r * 255) | 0},${(u.g * 255) | 0},${(u.b * 255) | 0})`,
      count: nInactive,
    });
  }
  ctx.renderLegend?.({
    title: cfg.layerLabel,
    swatches,
    note: `${nActive} active / ${n} total points`,
  });

  ctx.renderJSON({
    artifact: payload?.name,
    n,
    n_active: nActive,
    bit_counts: Object.fromEntries(Array.from(bitCounts).map((c, i) => [`bit${i}`, c])),
    layer: cfg.layerId,
  });
}

function collect(ctx, payload, xyz) {
  if (!payload) return null;
  const kind = payload.kind;
  if (kind === AK_POINT_COLUMN) {
    const arr = payload.data;
    const n = Math.min(payload.n_items ?? arr.length, xyz.length / 3 | 0);
    const positions = new Float64Array(n * 3);
    const bytes = new Uint8Array(n);
    for (let i = 0; i < n; i++) {
      positions[i * 3 + 0] = xyz[i * 3 + 0];
      positions[i * 3 + 1] = xyz[i * 3 + 1];
      positions[i * 3 + 2] = xyz[i * 3 + 2];
      bytes[i] = arr[i];
    }
    return { positions, bytes, n };
  }
  if (kind === AK_ARRAY_INLINE) {
    const items = payload.items || [];
    const buildings = ctx.getSiblingDecoded?.('per-building-records');
    if (!buildings?.items) {
      ctx.renderHTML(
        `<div class="render-placeholder">${payload.name}: subscribe per-building-records.</div>`,
      );
      return null;
    }
    let total = 0;
    for (let bi = 0; bi < items.length; bi++) {
      const u8 = items[bi];
      if (!u8 || u8.byteLength === 0) continue;
      total += u8.byteLength;
    }
    if (total === 0) return { positions: new Float64Array(0), bytes: new Uint8Array(0), n: 0 };

    const positions = new Float64Array(total * 3);
    const bytes = new Uint8Array(total);
    let cursor = 0;
    for (let bi = 0; bi < items.length; bi++) {
      const u8 = items[bi];
      if (!u8 || u8.byteLength === 0) continue;
      const u8View = new Uint8Array(u8.buffer, u8.byteOffset, u8.byteLength);
      const hdr = parseBuildingHeader(buildings.items[bi]);
      if (!hdr || hdr.n_pts !== u8.byteLength) continue;
      for (let i = 0; i < hdr.n_pts; i++) {
        const g = hdr.globalIndices[i];
        positions[cursor * 3 + 0] = xyz[g * 3 + 0];
        positions[cursor * 3 + 1] = xyz[g * 3 + 1];
        positions[cursor * 3 + 2] = xyz[g * 3 + 2];
        bytes[cursor] = u8View[i];
        cursor++;
      }
    }
    return {
      positions: positions.subarray(0, cursor * 3),
      bytes: bytes.subarray(0, cursor),
      n: cursor,
    };
  }
  return null;
}

function computeOrigin(positions, sample) {
  let sx = 0, sy = 0, sz = 0;
  for (let i = 0; i < sample; i++) {
    sx += positions[i * 3 + 0];
    sy += positions[i * 3 + 1];
    sz += positions[i * 3 + 2];
  }
  return { x: sx / sample, y: sy / sample, z: sz / sample };
}
