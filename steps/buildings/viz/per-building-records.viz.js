// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: per-building-records (artifact written by extract-buildings).
//
// Phase 1 of the roof-recon evaluation. Renders every extracted building
// in a distinct hue: footprint as a closed line ring on the ground plane,
// inlier points coloured the same hue. Reads lidar-positions as a sibling
// to dereference the per-record point indices into actual point coords.
//
// Per-record binary layout (must match extract-buildings.c HEADER_BYTES = 68):
//
//   offset  type        field
//   ──────  ──────────  ────────────────────────────────────────────────
//   0       u32         schema_version       = 1
//   4       u32         footprint_vertex_count    (N, ring is OPEN)
//   8       u32         point_index_count         (M)
//   12      f64 [6]     bbox  (xmin,ymin,zmin,xmax,ymax,zmax)  (L93)
//   60      f64         ground_z                                (L93)
//   68      f64 [N][3]  footprint                               (L93)
//   68+24N  u32 [M]     point_indices  (indices into lidar-positions, f64×3)
//
// Coordinate frame: everything is in raw L93 (Lambert-93 metres + altitude),
// the same frame lidar-positions emits. Three.js can't render at L93
// magnitudes (~355 000 east, ~6 689 000 north on Nantes — float32 precision
// breaks). We pick an origin from the union bbox of all records and
// subtract it before pushing buffers; the choice is local to this viz
// and doesn't propagate anywhere else.

import * as THREE from 'three';

// HSL spread — give every building a distinct, saturated colour. Golden-
// angle hue increment keeps adjacent indices visually separable.
const GOLDEN_ANGLE_DEG = 137.508;

function hueFor(idx) {
  return (idx * GOLDEN_ANGLE_DEG) % 360;
}

function colorFor(idx) {
  const c = new THREE.Color();
  c.setHSL(hueFor(idx) / 360, 0.75, 0.55);
  return c;
}

function parseRecord(u8) {
  // u8 is a Uint8Array view of one record's bytes (length-prefix already
  // stripped by the AK_ARRAY_INLINE decoder).
  if (u8.byteLength < 68) return null;
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const schema = dv.getUint32(0, true);
  if (schema !== 1) return null;
  const nVerts = dv.getUint32(4, true);
  const nIdx = dv.getUint32(8, true);
  const bbox = [
    dv.getFloat64(12, true), dv.getFloat64(20, true), dv.getFloat64(28, true),
    dv.getFloat64(36, true), dv.getFloat64(44, true), dv.getFloat64(52, true),
  ];
  const groundZ = dv.getFloat64(60, true);
  const fpOffset = 68;
  const idxOffset = fpOffset + nVerts * 3 * 8;
  const expected = idxOffset + nIdx * 4;
  if (expected > u8.byteLength) return null;
  // Float64Array view over the footprint section — must use a copy if
  // the byteOffset isn't 8-aligned (DataView handles unaligned reads;
  // typed-array constructors do not). The AK_ARRAY_INLINE decoder hands
  // us subarrays of the underlying mmap that are usually 4-aligned only.
  let footprint;
  if ((u8.byteOffset + fpOffset) % 8 === 0) {
    footprint = new Float64Array(u8.buffer, u8.byteOffset + fpOffset, nVerts * 3);
  } else {
    footprint = new Float64Array(nVerts * 3);
    for (let i = 0; i < nVerts * 3; i++) {
      footprint[i] = dv.getFloat64(fpOffset + i * 8, true);
    }
  }
  // u32 indices are 4-byte aligned at idxOffset = 68 + 24N which is
  // always 4-aligned regardless of u8.byteOffset alignment. Use the
  // typed-array view directly for speed.
  const indices = new Uint32Array(u8.buffer, u8.byteOffset + idxOffset, nIdx);
  return { schema, nVerts, nIdx, bbox, groundZ, footprint, indices };
}

export function render(ctx, decoded) {
  // decoded shape (artifact-decoder.decodeArrayInline):
  //   { kind, n, stride, items: Uint8Array[], items_json: any[] }
  const items = decoded?.items;
  if (!Array.isArray(items) || items.length === 0) {
    ctx.renderHTML(`<div class="render-placeholder">no buildings extracted</div>`);
    return;
  }

  // Need lidar-positions to dereference indices. If the user hasn't
  // subscribed to it, fall back to footprint-only.
  const positionsArtifact = ctx.getSiblingDecoded?.('lidar-positions');
  const havePoints =
    positionsArtifact && positionsArtifact.kind === 10 && positionsArtifact.data;
  const xyz = havePoints ? positionsArtifact.data : null;

  const records = items.map(parseRecord).filter(Boolean);
  if (records.length === 0) {
    ctx.renderJSON({ artifact: 'per-building-records', n: items.length, parsed: 0 });
    return;
  }

  // Honour the scene-origin contract — share the frame with the
  // lidar-positions composite + any candidate-mesh layers (so toggles
  // line up at the same camera framing). If we're first to render,
  // pick the union-bbox centroid as the shared origin.
  let origin = ctx.viewer.getSceneOrigin?.();
  if (!origin) {
    let ux = 0, uy = 0, uz = 0, nb = 0;
    for (const r of records) {
      ux += (r.bbox[0] + r.bbox[3]) * 0.5;
      uy += (r.bbox[1] + r.bbox[4]) * 0.5;
      uz += (r.bbox[2] + r.bbox[5]) * 0.5;
      nb++;
    }
    if (nb === 0) return;
    origin = { x: ux / nb, y: uy / nb, z: uz / nb };
    ctx.viewer.setSceneOrigin?.(origin);
  }
  const ox = origin.x, oy = origin.y, oz = origin.z;

  // ── Footprint rings (one LineSegments object total)
  let totalRingSegs = 0;
  for (const r of records) totalRingSegs += r.nVerts;
  const ringPos = new Float32Array(totalRingSegs * 6);
  const ringCol = new Float32Array(totalRingSegs * 6);
  let rOff = 0;
  records.forEach((r, ri) => {
    const c = colorFor(ri);
    for (let i = 0; i < r.nVerts; i++) {
      const j = (i + 1) % r.nVerts;
      ringPos[rOff + 0] = r.footprint[3 * i + 0] - ox;
      ringPos[rOff + 1] = r.footprint[3 * i + 1] - oy;
      ringPos[rOff + 2] = r.footprint[3 * i + 2] - oz;
      ringPos[rOff + 3] = r.footprint[3 * j + 0] - ox;
      ringPos[rOff + 4] = r.footprint[3 * j + 1] - oy;
      ringPos[rOff + 5] = r.footprint[3 * j + 2] - oz;
      ringCol[rOff + 0] = ringCol[rOff + 3] = c.r;
      ringCol[rOff + 1] = ringCol[rOff + 4] = c.g;
      ringCol[rOff + 2] = ringCol[rOff + 5] = c.b;
      rOff += 6;
    }
  });
  const ringGeom = new THREE.BufferGeometry();
  ringGeom.setAttribute('position', new THREE.BufferAttribute(ringPos, 3));
  ringGeom.setAttribute('color', new THREE.BufferAttribute(ringCol, 3));
  const ringMat = new THREE.LineBasicMaterial({
    vertexColors: true,
    transparent: true,
    opacity: 0.95,
    depthWrite: false,
  });
  const ringLines = new THREE.LineSegments(ringGeom, ringMat);
  ringLines.name = 'per-building-footprints';
  // Vertices already in scene-local frame; don't translate.
  ctx.render3D({
    kind: 'lines',
    object: ringLines,
    layer: { id: 'building-footprints', label: 'Building footprints', defaultVisible: false },
  });

  // ── Inlier points (one Points object, coloured by building idx)
  if (havePoints) {
    let totalPts = 0;
    for (const r of records) totalPts += r.nIdx;
    const ptPos = new Float32Array(totalPts * 3);
    const ptCol = new Float32Array(totalPts * 3);
    let pOff = 0;
    records.forEach((r, ri) => {
      const c = colorFor(ri);
      for (let i = 0; i < r.nIdx; i++) {
        const idx = r.indices[i];
        // lidar-positions is f64×3 interleaved; positionsArtifact.data is
        // a Float64Array of length 3*N_points.
        const base = idx * 3;
        ptPos[pOff + 0] = xyz[base + 0] - ox;
        ptPos[pOff + 1] = xyz[base + 1] - oy;
        ptPos[pOff + 2] = xyz[base + 2] - oz;
        ptCol[pOff + 0] = c.r;
        ptCol[pOff + 1] = c.g;
        ptCol[pOff + 2] = c.b;
        pOff += 3;
      }
    });
    const ptGeom = new THREE.BufferGeometry();
    ptGeom.setAttribute('position', new THREE.BufferAttribute(ptPos, 3));
    ptGeom.setAttribute('color', new THREE.BufferAttribute(ptCol, 3));
    const ptMat = new THREE.PointsMaterial({
      vertexColors: true,
      size: 0.6,
      sizeAttenuation: true,
      transparent: true,
      opacity: 0.85,
      depthWrite: false,
    });
    const points = new THREE.Points(ptGeom, ptMat);
    points.name = 'per-building-points';
    // Vertices already in scene-local frame; don't translate.
    ctx.render3D({
      kind: 'points',
      object: points,
      layer: { id: 'building-points', label: 'Per-building points', defaultVisible: false },
    });

    ctx.onCleanup(() => {
      ringGeom.dispose(); ringMat.dispose();
      ptGeom.dispose(); ptMat.dispose();
    });

    // Frame on the union bbox the first time we render.
    if (!ctx.viewer.__bldgFramed) {
      const box = new THREE.Box3();
      // Box already in scene-local frame.
      box.setFromBufferAttribute(new THREE.BufferAttribute(ptPos, 3));
      ctx.viewer.frameBox?.(box);
      ctx.viewer.__bldgFramed = true;
      ctx.onCleanup(() => { ctx.viewer.__bldgFramed = false; });
    }
  } else {
    ctx.onCleanup(() => { ringGeom.dispose(); ringMat.dispose(); });
  }

  // Stats sidebar.
  ctx.renderJSON({
    artifact: 'per-building-records',
    n_buildings: records.length,
    total_inlier_points: records.reduce((s, r) => s + r.nIdx, 0),
    total_footprint_vertices: records.reduce((s, r) => s + r.nVerts, 0),
    sibling_lidar_positions: havePoints
      ? `subscribed (${positionsArtifact.n_items.toLocaleString()} pts)`
      : 'not subscribed — points not rendered, footprints only',
    local_origin_l93: { x: ox, y: oy, z: oz },
  });
}
