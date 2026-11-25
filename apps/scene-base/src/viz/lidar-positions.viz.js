// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// t() — Phase 1 obfuscation marker; see viewer-sdk/src/viewer.js.
const t = (s) => s;

// lidar-positions.viz.js — fast preview point cloud (streaming).
//
// Reads:
//   lidar-positions  AK_POINT_COLUMN F64×3 (x, y, z) interleaved, in L93.
//
// Two-mode renderer:
//
//   1. Streaming (preferred — laz-extract-disc emits chunks while reading).
//      beginStream() preps state; appendChunk() extends a growable Points
//      layer in place; finalizeStream() writes the final summary.
//
//   2. One-shot (fallback for cached re-runs that arrive as a single
//      ARTIFACT_HEADER + binary frame). render() builds the whole layer at
//      once.
//
// Both modes converge on a single layer leaf 'lidar-positions-preview'.
// The first chunk picks (or adopts) the scene-origin so subsequent layers
// share a coordinate frame.

import * as THREE from 'three';

export const meta = {
  name: 'LiDAR positions — preview cloud',
  requires: ['lidar-positions'],
  pipelines: '*',
  default: true,
};

const PREVIEW_INITIAL_CAPACITY = 200_000;
const POINT_COLOR = 0x2f81f7;
const LAYER_ID = 'lidar-positions-preview';
const LAYER_LABEL = 'LiDAR positions (preview)';

// ─── One-shot path ───────────────────────────────────────────────────

export function render(ctx, decoded) {
  const payload = decoded['lidar-positions'];
  if (!payload?.data || !payload.n_items) return;
  const xyz = payload.data; // F64 interleaved
  const n = payload.n_items;

  let origin = ctx.viewer.getSceneOrigin();
  if (!origin) {
    origin = computeCentroid(xyz, n);
    ctx.viewer.setSceneOrigin(origin);
  }
  const positions = recenter(xyz, n, origin);

  // Build the THREE.Points pre-built so the SDK ctx (which requires
  // spec.object) and lab-ui's ctx (which would otherwise rebuild via
  // viewer.addLayer's 'points' kind) both work uniformly.
  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  // Pre-set the bounding sphere so Three.js' lazy compute (which would
  // walk the whole array on first render and choke on any NaN) never runs.
  setBoundingSphereFromRange(geom, n);
  // World-unit size + sizeAttenuation:true — points scale with zoom
  // like other 3D geometry. 0.1 m ≈ 1 lidar shot footprint at typical
  // 1 m point spacing; visible at close zoom, vanishes at far.
  const mat = new THREE.PointsMaterial({
    size: 0.1,
    sizeAttenuation: true,
    color: POINT_COLOR,
  });
  const points = new THREE.Points(geom, mat);
  points.name = LAYER_ID;

  ctx.render3D({
    kind: 'points',
    object: points,
    layer: { id: LAYER_ID, label: LAYER_LABEL, defaultVisible: false },
  });

  ctx.onCleanup(() => {
    geom.dispose();
    mat.dispose();
  });
}

// ─── Streaming path ───────────────────────────────────────────────────

export function beginStream(ctx, _info) {
  ctx.__streamState = {
    geom: null,
    points: null,
    mat: null,
    capacity: PREVIEW_INITIAL_CAPACITY,
    count: 0,
    origin: null,
  };
}

export function appendChunk(ctx, decoded, info) {
  const st = ctx.__streamState;
  if (!st) return;
  // Decoded shape from artifact-decoder.decodePointColumn for streaming chunks.
  if (decoded?.name !== 'lidar-positions' || !decoded.data) return;
  const xyz = decoded.data;
  const n = decoded.n_items;
  // Empty chunks (n=0) are legitimate sentinels: laz-extract-disc emits
  // a final=true chunk with zero bytes when the total point count is a
  // multiple of CHUNK_POINTS — see steps/copc/laz-extract-disc.c around
  // line 102-110. Skip the geometry-update body but let orchestration
  // proceed to call finalizeStream with the existing valid buffer.
  if (n <= 0) return;

  if (st.geom == null) {
    let origin = ctx.viewer.getSceneOrigin();
    if (!origin) {
      origin = computeCentroid(xyz, n);
      ctx.viewer.setSceneOrigin(origin);
    }
    st.origin = origin;

    const f32 = new Float32Array(st.capacity * 3);
    const initial = recenter(xyz, n, origin);
    f32.set(initial);
    st.geom = new THREE.BufferGeometry();
    const posAttr = new THREE.BufferAttribute(f32, 3);
    posAttr.setUsage(THREE.DynamicDrawUsage);
    st.geom.setAttribute('position', posAttr);
    st.geom.setDrawRange(0, n);
    // Pre-set the bounding sphere so Three.js' lazy compute on the first
    // frame doesn't iterate the unwritten capacity tail (zeros are fine
    // but NaN values anywhere in the buffer would poison the radius).
    setBoundingSphereFromRange(st.geom, n);
    st.mat = new THREE.PointsMaterial({
      size: 0.1,
      sizeAttenuation: true,
      color: POINT_COLOR,
    });
    st.points = new THREE.Points(st.geom, st.mat);
    st.points.name = LAYER_ID;
    st.count = n;

    ctx.render3D({
      kind: 'points',
      object: st.points,
      layer: { id: LAYER_ID, label: LAYER_LABEL, defaultVisible: false },
    });

    ctx.onCleanup(() => {
      st.geom?.dispose();
      st.mat?.dispose();
    });
  } else {
    const newCount = st.count + n;
    if (newCount > st.capacity) {
      // Grow the underlying buffer geometrically (1.6x), preserve old data.
      const newCap = Math.max(newCount, Math.floor(st.capacity * 1.6));
      const old = st.geom.attributes.position;
      const bigger = new Float32Array(newCap * 3);
      bigger.set(old.array);
      const newAttr = new THREE.BufferAttribute(bigger, 3);
      newAttr.setUsage(THREE.DynamicDrawUsage);
      st.geom.setAttribute('position', newAttr);
      st.capacity = newCap;
    }
    const posAttr = st.geom.attributes.position;
    const offset = st.count * 3;
    // Recenter directly into the destination range.
    for (let i = 0; i < n; i++) {
      posAttr.array[offset + i * 3 + 0] = xyz[i * 3 + 0] - st.origin.x;
      posAttr.array[offset + i * 3 + 1] = xyz[i * 3 + 1] - st.origin.y;
      posAttr.array[offset + i * 3 + 2] = xyz[i * 3 + 2] - st.origin.z;
    }
    posAttr.addUpdateRange(offset, n * 3);
    posAttr.needsUpdate = true;
    st.count = newCount;
    st.geom.setDrawRange(0, st.count);
    // Three.js' computeBoundingSphere walks the entire underlying array
    // and ignores drawRange — that would feed it the unwritten zero tail
    // of our preallocated buffer (or worse, NaN). Compute manually over
    // [0, st.count) instead.
    setBoundingSphereFromRange(st.geom, st.count);
  }

  if (info?.final) {
    // finalizeStream will be called next; nothing extra here.
  }
}

export function finalizeStream(ctx, _info) {
  const st = ctx.__streamState;
  if (!st?.geom) return;
  setBoundingSphereFromRange(st.geom, st.count);
}

// Compute boundingSphere over the first `count` vertices only (valid
// streaming range). Skips vertices with NaN coords. Mirrors the math in
// THREE.BufferGeometry.computeBoundingSphere but bounded.
function setBoundingSphereFromRange(geom, count) {
  if (count <= 0) {
    geom.boundingSphere = new THREE.Sphere(new THREE.Vector3(0, 0, 0), 0);
    return;
  }
  const arr = geom.attributes.position.array;
  // First pass: centroid.
  let cx = 0, cy = 0, cz = 0, valid = 0;
  for (let i = 0; i < count; i++) {
    const x = arr[i * 3 + 0], y = arr[i * 3 + 1], z = arr[i * 3 + 2];
    if (Number.isNaN(x) || Number.isNaN(y) || Number.isNaN(z)) continue;
    cx += x; cy += y; cz += z; valid++;
  }
  if (valid === 0) {
    geom.boundingSphere = new THREE.Sphere(new THREE.Vector3(0, 0, 0), 0);
    return;
  }
  cx /= valid; cy /= valid; cz /= valid;
  // Second pass: max squared distance.
  let r2 = 0;
  for (let i = 0; i < count; i++) {
    const x = arr[i * 3 + 0], y = arr[i * 3 + 1], z = arr[i * 3 + 2];
    if (Number.isNaN(x) || Number.isNaN(y) || Number.isNaN(z)) continue;
    const dx = x - cx, dy = y - cy, dz = z - cz;
    const d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > r2) r2 = d2;
  }
  geom.boundingSphere = new THREE.Sphere(new THREE.Vector3(cx, cy, cz), Math.sqrt(r2));
}

// ─── Helpers ──────────────────────────────────────────────────────────

function computeCentroid(xyz, n) {
  if (n <= 0) return { x: 0, y: 0, z: 0 };
  let sx = 0, sy = 0, sz = 0;
  for (let i = 0; i < n; i++) {
    sx += xyz[i * 3 + 0];
    sy += xyz[i * 3 + 1];
    sz += xyz[i * 3 + 2];
  }
  return { x: sx / n, y: sy / n, z: sz / n };
}

function recenter(xyz, n, origin) {
  const out = new Float32Array(n * 3);
  // Sanitize: if a source coord is NaN/non-finite (server-side bug or
  // incomplete chunk), emit (0,0,0) so Three.js' bounding-sphere math
  // stays well-defined. The legitimate scene always has at least one
  // valid point at the origin, so this is a safe fallback.
  for (let i = 0; i < n; i++) {
    const x = xyz[i * 3 + 0];
    const y = xyz[i * 3 + 1];
    const z = xyz[i * 3 + 2];
    if (Number.isFinite(x) && Number.isFinite(y) && Number.isFinite(z)) {
      out[i * 3 + 0] = x - origin.x;
      out[i * 3 + 1] = y - origin.y;
      out[i * 3 + 2] = z - origin.z;
    }
  }
  return out;
}
