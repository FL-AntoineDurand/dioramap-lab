// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Built-in point-column fallback.
//
// L11 onwards, the heavy lifting (3D point rendering, per-class /
// per-value histograms, normal-coloured points) lives in step-visualizers
// at v3/steps/<step>/viz/<artifact>.viz.js — see VISUALIZERS.md §5.
// This builtin handles AK_POINT_COLUMN artifacts that DON'T have a
// step-visualizer: it emits a small JSON stats block so the artifact
// inspector row isn't empty, and silently no-ops on the 3D / chart /
// SVG surfaces. Anything richer is a step-visualizer's job.
//
// Streaming triplet stays here too — chunks for step-visualizer'd
// artifacts (e.g. lidar-positions) are routed to the step-visualizer's
// own beginStream/appendChunk/finalizeStream by the inspector before
// this fallback ever sees them. Only chunked artifacts WITHOUT a
// step-visualizer would land here, in which case we skip rendering
// (ctx.__streamState.noop=true) and let the artifact's trailing
// ARTIFACT_HEADER fall through to render() if needed.

export function beginStream(ctx, _info) {
  ctx.__streamState = { noop: true };
}

export function appendChunk(_ctx, _data, _info) { /* no-op */ }

export function finalizeStream(_ctx, _info) { /* no-op */ }

export function render(ctx, data) {
  const { name, dtype, components, n_items, data: arr } = data;
  // Cheap stats — count + range + mean for scalar columns; just
  // dimensions for multi-component.
  const summary = {
    artifact: name,
    dtype,
    components,
    n_items,
  };
  if (components === 1 && arr && typeof arr.length === 'number') {
    summary.stats = computeScalarStats(arr);
  }
  ctx.renderJSON(summary);
}

function computeScalarStats(arr) {
  const n = arr.length;
  if (n === 0) return { n: 0 };
  let min = Infinity, max = -Infinity, sum = 0;
  for (let i = 0; i < n; i++) {
    const v = arr[i];
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
  }
  return { n, min, max, mean: sum / n };
}
