// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: lidar-positions (artifact written by laz-extract-disc).
//
// Inspector-side metadata + streaming progress overlay. The actual 3D
// point cloud is painted by the composite-visualizer in
// `@dioramap/scene-base/viz/lidar-positions.viz.js` (the
// `LiDAR positions (preview)` composite owned by the composite-views
// panel — same module the SDK uses). This step-visualizer used to also
// render a 3D layer, but that produced a duplicate in the layer list,
// so it now only emits the JSON stats block (origin, bbox, n_points,
// dtype) and a streaming-progress overlay line.

export function beginStream(ctx, { artifact }) {
  if (artifact !== 'lidar-positions') {
    ctx.__streamState = { noop: true };
    return;
  }
  const el = document.createElement('div');
  el.className = 'streaming-meta mono';
  ctx.renderHTML(el);
  ctx.__streamState = {
    n_items: 0,
    overlayMetaEl: el,
  };
}

export function appendChunk(ctx, data, { seq, final, n_items_total }) {
  const st = ctx.__streamState;
  if (!st || st.noop) return;
  if (data.kind !== 10 || data.name !== 'lidar-positions') return;

  st.n_items = n_items_total ?? (st.n_items + data.n_items);
  if (st.overlayMetaEl) {
    st.overlayMetaEl.textContent =
      `streaming lidar-positions · chunk ${seq}${final ? ' (final)' : ''} · ${st.n_items.toLocaleString()} points so far`;
  }
}

export function finalizeStream(ctx, { n_items, hash }) {
  const st = ctx.__streamState;
  if (!st || st.noop) return;
  if (st.overlayMetaEl) {
    st.overlayMetaEl.textContent =
      `lidar-positions · ${n_items.toLocaleString()} points · hash ${hash?.slice(0, 8) ?? ''}`;
  }
  ctx.renderJSON({
    artifact: 'lidar-positions',
    n_points: n_items,
    dtype: 'f64',
    streamed: true,
  });
}

export function render(ctx, data) {
  // One-shot path (cache re-runs deliver as a single ARTIFACT_HEADER).
  const { name, n_items, data: xyz, dtype } = data;

  // Read scene origin if the composite or another layer has set one;
  // otherwise compute a centroid for display only (no setSceneOrigin —
  // the composite owns that).
  const sceneOrigin = ctx.viewer.getSceneOrigin?.();
  const origin = sceneOrigin ?? computeCentroid(xyz);

  let xMin = Infinity, xMax = -Infinity;
  let yMin = Infinity, yMax = -Infinity;
  let zMin = Infinity, zMax = -Infinity;
  for (let i = 0; i < n_items; i++) {
    const x = xyz[3 * i + 0] - origin.x;
    const y = xyz[3 * i + 1] - origin.y;
    const z = xyz[3 * i + 2] - origin.z;
    if (x < xMin) xMin = x; if (x > xMax) xMax = x;
    if (y < yMin) yMin = y; if (y > yMax) yMax = y;
    if (z < zMin) zMin = z; if (z > zMax) zMax = z;
  }

  ctx.renderJSON({
    artifact: name,
    n_points: n_items,
    dtype,
    origin_l93: { x: origin.x, y: origin.y, z: origin.z },
    bbox_local: {
      x: [xMin, xMax],
      y: [yMin, yMax],
      z: [zMin, zMax],
    },
  });
}

function computeCentroid(xyz) {
  let sx = 0, sy = 0, sz = 0;
  const n = Math.floor(xyz.length / 3);
  if (n === 0) return { x: 0, y: 0, z: 0 };
  for (let i = 0; i < n; i++) {
    sx += xyz[3 * i + 0];
    sy += xyz[3 * i + 1];
    sz += xyz[3 * i + 2];
  }
  return { x: sx / n, y: sy / n, z: sz / n };
}
