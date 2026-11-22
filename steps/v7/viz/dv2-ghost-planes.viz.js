// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: dv2-ghost-planes — pre-GSR ghost-plane seeds.
//
// One per-building ARRAY_INLINE record per pipeline run, each
// containing a flat sequence of `[u32 record_len][record bytes]`
// fixed-stride 104-byte header + variable hull + inlier_indices.
//
// Per-record schema (matches v7-density-voids-2.c::pack_ghost_plane):
//   f64×3 normal, f64 d, f64 tilt, f64 azimuth, f64 rms,
//   f64×3 centroid, f64 area,
//   u32 void_id, u32 inlier_count, u32 hull_vert_count, u32 _reserved,
//   f64×3·hull_vert_count hull,
//   u32×inlier_count seed_indices (LOCAL to building)
//
// Renders:
//   - hull polygon as a closed THREE.Line per ghost (golden-angle
//     palette per void id)
//   - inlier point cloud as a THREE.Points (same hue, opaque)
//   - legend: per-ghost row (hue swatch + tilt + inlier count)

import * as THREE from 'three';

const AK_ARRAY_INLINE = 8;
const HEADER_BYTES = 104;
const GOLDEN_ANGLE_DEG = 137.508;

function paletteColor(id) {
  const c = new THREE.Color();
  const hue = ((id * GOLDEN_ANGLE_DEG) % 360) / 360;
  c.setHSL(hue, 0.78, 0.55);
  return c;
}

function parsePerBldgBlob(u8) {
  // Per-bldg blob uses the 8-aligned framing produced by blob_push in
  // <v3/blob.h>: `{u32 rec_len, u32 zero_pad, payload, pad_to_8}` × N.
  if (!u8 || u8.byteLength < 8) return [];
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const ghosts = [];
  let off = 0;
  while (off + 8 <= u8.byteLength) {
    const recLen = dv.getUint32(off, true);
    off += 8;
    if (recLen < HEADER_BYTES || off + recLen > u8.byteLength) break;
    const base = off;
    const normal = [
      dv.getFloat64(base + 0, true),
      dv.getFloat64(base + 8, true),
      dv.getFloat64(base + 16, true),
    ];
    const d = dv.getFloat64(base + 24, true);
    const tilt = dv.getFloat64(base + 32, true);
    const azimuth = dv.getFloat64(base + 40, true);
    const rms = dv.getFloat64(base + 48, true);
    const centroid = [
      dv.getFloat64(base + 56, true),
      dv.getFloat64(base + 64, true),
      dv.getFloat64(base + 72, true),
    ];
    const area = dv.getFloat64(base + 80, true);
    const voidId = dv.getUint32(base + 88, true);
    const inlierCount = dv.getUint32(base + 92, true);
    const hullVertCount = dv.getUint32(base + 96, true);
    let p = base + HEADER_BYTES;
    const hull = new Float64Array(hullVertCount * 3);
    for (let k = 0; k < hullVertCount * 3; k++) hull[k] = dv.getFloat64(p + k * 8, true);
    p += hullVertCount * 24;
    const inliers = new Uint32Array(inlierCount);
    for (let k = 0; k < inlierCount; k++) inliers[k] = dv.getUint32(p + k * 4, true);
    ghosts.push({ normal, d, tilt, azimuth, rms, centroid, area, voidId,
                  inlierCount, hullVertCount, hull, inliers });
    off += (recLen + 7) & ~7;
  }
  return ghosts;
}

const GHOST_PATH = ['Ghost Planes (pre-GSR)'];

export function render(ctx, data) {
  if (data?.kind !== AK_ARRAY_INLINE || !Array.isArray(data.items)) {
    ctx.renderHTML(`<div class="render-placeholder">dv2-ghost-planes expected ARRAY_INLINE.</div>`);
    return;
  }

  // Sibling reads (used to map LOCAL ghost inlier indices → 3D
  // positions). lidar-positions for the F64 globals; per-building-
  // records for the local→global header.
  const positionsArt = ctx.getSiblingDecoded?.('lidar-positions');
  const buildingsArt = ctx.getSiblingDecoded?.('per-building-records');
  if (!positionsArt?.data || !buildingsArt?.items) {
    ctx.renderHTML(
      `<div class="render-placeholder">dv2-ghost-planes: subscribe lidar-positions + per-building-records to render hulls + inlier clouds.</div>`,
    );
    return;
  }
  const xyz = positionsArt.data;

  let origin = ctx.viewer.getSceneOrigin?.();
  if (!origin) {
    // Late-arriving — use median building centroid (cheap fallback).
    origin = { x: xyz[0], y: xyz[1], z: xyz[2] };
    ctx.viewer.setSceneOrigin?.(origin);
  }

  // Aggregate across buildings for the legend.
  const swatches = [];
  let totalGhosts = 0, totalInliers = 0;

  for (let bi = 0; bi < data.items.length; bi++) {
    const ghosts = parsePerBldgBlob(data.items[bi]);
    if (ghosts.length === 0) continue;
    const bldg = buildingsArt.items[bi];
    if (!bldg || bldg.byteLength < 12) continue;
    const dv = new DataView(bldg.buffer, bldg.byteOffset, bldg.byteLength);
    const nVerts = dv.getUint32(4, true);
    const nPts   = dv.getUint32(8, true);
    const idxOff = 68 + nVerts * 24;
    if (idxOff + nPts * 4 > bldg.byteLength) continue;
    const localToGlobal = new Uint32Array(bldg.buffer, bldg.byteOffset + idxOff, nPts);

    for (const g of ghosts) {
      const colour = paletteColor(g.voidId);
      // Hull line (closed polygon).
      const hullPts = new Float32Array(g.hullVertCount * 3 + 3);
      for (let k = 0; k < g.hullVertCount; k++) {
        hullPts[k * 3 + 0] = g.hull[k * 3 + 0] - origin.x;
        hullPts[k * 3 + 1] = g.hull[k * 3 + 1] - origin.y;
        hullPts[k * 3 + 2] = g.hull[k * 3 + 2] - origin.z;
      }
      // Close: copy first vertex to the end.
      hullPts[g.hullVertCount * 3 + 0] = hullPts[0];
      hullPts[g.hullVertCount * 3 + 1] = hullPts[1];
      hullPts[g.hullVertCount * 3 + 2] = hullPts[2];
      const lineGeom = new THREE.BufferGeometry();
      lineGeom.setAttribute('position', new THREE.BufferAttribute(hullPts, 3));
      const lineMat = new THREE.LineBasicMaterial({
        color: colour, linewidth: 2, transparent: true, opacity: 0.9,
      });
      const line = new THREE.Line(lineGeom, lineMat);
      line.name = `ghost-${bi}-${g.voidId}-hull`;

      // Inlier cloud.
      const cloudPos = new Float32Array(g.inlierCount * 3);
      const cloudCol = new Float32Array(g.inlierCount * 3);
      for (let i = 0; i < g.inlierCount; i++) {
        const local = g.inliers[i];
        const gi = localToGlobal[local];
        cloudPos[i * 3 + 0] = xyz[gi * 3 + 0] - origin.x;
        cloudPos[i * 3 + 1] = xyz[gi * 3 + 1] - origin.y;
        cloudPos[i * 3 + 2] = xyz[gi * 3 + 2] - origin.z;
        cloudCol[i * 3 + 0] = colour.r;
        cloudCol[i * 3 + 1] = colour.g;
        cloudCol[i * 3 + 2] = colour.b;
      }
      const cloudGeom = new THREE.BufferGeometry();
      cloudGeom.setAttribute('position', new THREE.BufferAttribute(cloudPos, 3));
      cloudGeom.setAttribute('color',    new THREE.BufferAttribute(cloudCol, 3));
      const cloudMat = new THREE.PointsMaterial({
        size: 3.0, sizeAttenuation: false, vertexColors: true,
      });
      const cloud = new THREE.Points(cloudGeom, cloudMat);
      cloud.name = `ghost-${bi}-${g.voidId}-inliers`;

      const layerLabel = `void ${g.voidId} (b${bi}, ${g.inlierCount} pts, tilt ${g.tilt.toFixed(0)}°)`;
      // addLayer accepts any THREE.Object3D, including Group. Pack the
      // hull line + inlier cloud into one group so they toggle together.
      const group = new THREE.Group();
      group.add(line);
      group.add(cloud);
      group.name = `ghost-${bi}-${g.voidId}`;
      ctx.render3D({
        kind: 'group',
        object: group,
        layer: { id: `dv2-ghost-planes/b${bi}/void${g.voidId}`,
                 label: layerLabel,
                 path: GHOST_PATH,
                 defaultVisible: false },
      });
      ctx.onCleanup(() => {
        lineGeom.dispose(); lineMat.dispose();
        cloudGeom.dispose(); cloudMat.dispose();
      });

      const c8 = (v) => Math.round(v * 255) | 0;
      swatches.push({
        label: layerLabel,
        color: `rgb(${c8(colour.r)},${c8(colour.g)},${c8(colour.b)})`,
        count: g.inlierCount,
      });
      totalGhosts++;
      totalInliers += g.inlierCount;
    }
  }

  ctx.renderLegend?.({
    title: 'Ghost Planes (pre-GSR)',
    swatches,
    note: `${totalGhosts} ghost(s), ${totalInliers} inliers total`,
  });

  ctx.renderJSON({
    artifact: 'dv2-ghost-planes',
    n_buildings: data.items.length,
    n_ghosts: totalGhosts,
    n_inliers_total: totalInliers,
    note: 'hull = convex hull of void cell centres lifted onto fitted plane; inliers = RANSAC-selected rim points.',
  });
}
