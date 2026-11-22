// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: lidar-normals (artifact written by compute-normals).
//
// Renders the F32×3N raw normals as a 3D point cloud coloured by
// |nx|, |ny|, |nz| → R, G, B. Refactored to delegate to the shared
// per-point-rgb-cloud helper (same module also serves
// v7-tile-signals' tile-normals output and any future per-point vector
// field that wants the abs-clamp-to-RGB convention).

import { renderPerPointRgbCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/per-point-rgb-cloud.js';

export function render(ctx, data) {
  // Hide the sibling positions' default blue cloud so the same points
  // aren't drawn twice. The composite-visualizer's preview cloud is
  // useful by itself but redundant once we're colouring by normals.
  ctx.removeSibling3DLayer?.('lidar-positions');

  renderPerPointRgbCloud(ctx, data, {
    inputType: 'abs-vec3-f32',
    layerLabel: 'lidar-normals',
    layerId: 'lidar-normals',
    pointSize: 2.0,
  });
}
