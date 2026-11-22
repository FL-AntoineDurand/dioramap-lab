// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Generic helper: render an Int16/Int32 per-point segment-id field as a
// 3D point cloud with golden-angle distinct hues per id.
//
// Sibling reads:
//   - lidar-positions  (F64×3 LAZ positions)
//   - per-building-records  (when payload is AK_ARRAY_INLINE — gives the
//                            global-index map per building)
//
// Config knobs:
//   { layerPath, layerLabel, layerId, pointSize, unassignedColor }
// `unassignedColor` is the colour for id == -1; defaults to a dim grey
// so unassigned points stay visible but recede.

import * as THREE from 'three';
import { parseBuildingHeader } from './per-building-records.js';

const AK_POINT_COLUMN = 10;
const AK_ARRAY_INLINE = 8;
const GOLDEN_ANGLE_DEG = 137.508;

function categoricalColor(id, lightness = 0.55) {
  const c = new THREE.Color();
  const hue = ((id * GOLDEN_ANGLE_DEG) % 360) / 360;
  c.setHSL(hue, 0.78, lightness);
  return c;
}

export function renderCategoricalCloud(ctx, payload, config) {
  const cfg = {
    layerPath: undefined,
    layerLabel: payload?.name ?? 'segments',
    layerId: payload?.name ?? 'categorical-cloud',
    pointSize: 0.1,
    unassignedColor: { r: 0.18, g: 0.20, b: 0.23 },
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
  const { positions, ids, globalIds, n } = collected;

  // Distinct id count (for legend).
  const idSet = new Set();
  for (let i = 0; i < n; i++) {
    const v = ids[i];
    if (v >= 0) idSet.add(v);
  }

  let origin = ctx.viewer.getSceneOrigin?.();
  if (!origin) {
    origin = computeOrigin(positions, Math.min(n, 1000));
    ctx.viewer.setSceneOrigin?.(origin);
  }
  const centred = new Float32Array(n * 3);
  const colors = new Float32Array(n * 3);
  for (let i = 0; i < n; i++) {
    centred[i * 3 + 0] = positions[i * 3 + 0] - origin.x;
    centred[i * 3 + 1] = positions[i * 3 + 1] - origin.y;
    centred[i * 3 + 2] = positions[i * 3 + 2] - origin.z;
    const id = ids[i];
    if (id < 0) {
      colors[i * 3 + 0] = cfg.unassignedColor.r;
      colors[i * 3 + 1] = cfg.unassignedColor.g;
      colors[i * 3 + 2] = cfg.unassignedColor.b;
    } else {
      const c = categoricalColor(id);
      colors[i * 3 + 0] = c.r;
      colors[i * 3 + 1] = c.g;
      colors[i * 3 + 2] = c.b;
    }
  }

  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.BufferAttribute(centred, 3));
  const colorAttr = new THREE.BufferAttribute(colors, 3);
  geom.setAttribute('color', colorAttr);
  const mat = new THREE.PointsMaterial({
    size: cfg.pointSize,
    sizeAttenuation: true,
    vertexColors: true,
  });
  const points = new THREE.Points(geom, mat);
  points.name = cfg.layerId;

  // Pick resolver: this cloud's vertex order is per-building cursor order
  // (AK_ARRAY_INLINE) or per-point order (AK_POINT_COLUMN), NOT the global
  // lidar-positions index. `globalIds` maps the local vertex index back to
  // the global point id so the picker can report both it and the
  // categorical id (the owner plane, for grm-owner). This is the resolver
  // that removes the by-hand tile→per-building decode.
  points.userData.pick = (hit) => {
    const i = hit.index;
    if (i == null || i < 0 || i >= n) return null;
    return { pointId: globalIds ? globalIds[i] : i, id: ids[i] };
  };

  const layerSpec = {
    id: cfg.layerId,
    label: cfg.layerLabel,
    defaultVisible: false,
  };
  if (cfg.layerPath?.length) layerSpec.path = cfg.layerPath;

  ctx.render3D({ kind: 'points', object: points, layer: layerSpec });
  ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });

  // Selection awareness (Phase 5.2-followup-C). When the SceneState's
  // selection slot toggles to a set of segmentIds, dim points whose id
  // isn't in the set. When cleared, restore the original colours.
  // Recoloring is in-place on the existing BufferAttribute (cheap; ~10
  // ms for 50K points) — no geometry rebuild.
  const sceneState = ctx.sceneState;
  if (sceneState && sceneState.selection) {
    const baseColors = new Float32Array(colors);
    const dimMul = 0.18;
    const applySelection = (sel) => {
      if (!sel || !sel.segmentIds || sel.segmentIds.length === 0) {
        colors.set(baseColors);
      } else {
        const sidSet = new Set(Array.from(sel.segmentIds));
        let nMatched = 0;
        for (let i = 0; i < n; i++) {
          if (sidSet.has(ids[i])) {
            colors[i * 3 + 0] = baseColors[i * 3 + 0];
            colors[i * 3 + 1] = baseColors[i * 3 + 1];
            colors[i * 3 + 2] = baseColors[i * 3 + 2];
            nMatched++;
          } else {
            colors[i * 3 + 0] = baseColors[i * 3 + 0] * dimMul;
            colors[i * 3 + 1] = baseColors[i * 3 + 1] * dimMul;
            colors[i * 3 + 2] = baseColors[i * 3 + 2] * dimMul;
          }
        }
        // eslint-disable-next-line no-console
        console.info('[categorical-cloud] selection applied', {
          layerId: cfg.layerId,
          requestedSegIds: Array.from(sel.segmentIds).slice(0, 8),
          nPoints: n,
          nMatched,
          source: sel.source,
        });
      }
      colorAttr.needsUpdate = true;
    };
    const onSelChanged = (ev) => applySelection(ev.detail);
    sceneState.addEventListener('selection-changed', onSelChanged);
    // eslint-disable-next-line no-console
    console.info('[categorical-cloud] listener attached', {
      layerId: cfg.layerId, nPoints: n,
    });
    applySelection(sceneState.selection.getSelection());
    ctx.onCleanup(() => sceneState.removeEventListener('selection-changed', onSelChanged));
  }

  // Legend: top-N most-populated ids + an "others" row + unassigned.
  // Distinct-id count can run into the hundreds — capping keeps the
  // panel readable while still showing the major groups.
  const idCounts = new Map();
  let nUnassigned = 0;
  for (let i = 0; i < n; i++) {
    const v = ids[i];
    if (v < 0) { nUnassigned++; continue; }
    idCounts.set(v, (idCounts.get(v) || 0) + 1);
  }
  const sortedIds = Array.from(idCounts.entries()).sort((a, b) => b[1] - a[1]);
  const topN = 12;
  const swatches = [];
  for (let i = 0; i < Math.min(topN, sortedIds.length); i++) {
    const [id, count] = sortedIds[i];
    const c = categoricalColor(id);
    swatches.push({
      label: `id ${id}`,
      color: `rgb(${(c.r * 255) | 0},${(c.g * 255) | 0},${(c.b * 255) | 0})`,
      count,
    });
  }
  if (sortedIds.length > topN) {
    const rest = sortedIds.slice(topN).reduce((s, [, c]) => s + c, 0);
    swatches.push({ label: `… ${sortedIds.length - topN} more ids`, color: '#444', count: rest });
  }
  if (nUnassigned > 0) {
    const u = cfg.unassignedColor;
    swatches.push({
      label: 'unassigned (-1)',
      color: `rgb(${(u.r * 255) | 0},${(u.g * 255) | 0},${(u.b * 255) | 0})`,
      count: nUnassigned,
    });
  }
  ctx.renderLegend?.({
    title: cfg.layerLabel,
    swatches,
    note: `${idSet.size} distinct id(s)`,
  });

  ctx.renderJSON({
    artifact: payload?.name,
    n,
    n_distinct_ids: idSet.size,
    n_unassigned: nUnassigned,
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
    const ids = new Int32Array(n);
    const globalIds = new Int32Array(n);
    for (let i = 0; i < n; i++) {
      positions[i * 3 + 0] = xyz[i * 3 + 0];
      positions[i * 3 + 1] = xyz[i * 3 + 1];
      positions[i * 3 + 2] = xyz[i * 3 + 2];
      ids[i] = arr[i];
      globalIds[i] = i;  // per-point column is aligned to lidar-positions
    }
    return { positions, ids, globalIds, n };
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
      total += u8.byteLength / 2 | 0; // Int16
    }
    if (total === 0) return { positions: new Float64Array(0), ids: new Int32Array(0), n: 0 };

    const positions = new Float64Array(total * 3);
    const ids = new Int32Array(total);
    const globalIds = new Int32Array(total);
    let cursor = 0;
    for (let bi = 0; bi < items.length; bi++) {
      const u8 = items[bi];
      if (!u8 || u8.byteLength === 0) continue;
      const i16 = new Int16Array(u8.buffer, u8.byteOffset, u8.byteLength / 2 | 0);
      const hdr = parseBuildingHeader(buildings.items[bi]);
      if (!hdr || hdr.n_pts !== i16.length) continue;
      for (let i = 0; i < hdr.n_pts; i++) {
        const g = hdr.globalIndices[i];
        positions[cursor * 3 + 0] = xyz[g * 3 + 0];
        positions[cursor * 3 + 1] = xyz[g * 3 + 1];
        positions[cursor * 3 + 2] = xyz[g * 3 + 2];
        ids[cursor] = i16[i];
        globalIds[cursor] = g;  // local vertex → global lidar-positions index
        cursor++;
      }
    }
    return {
      positions: positions.subarray(0, cursor * 3),
      ids: ids.subarray(0, cursor),
      globalIds: globalIds.subarray(0, cursor),
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
