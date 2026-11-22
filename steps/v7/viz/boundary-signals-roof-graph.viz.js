// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: boundary-signals-roof-graph — section-aware
// roof-graph renderer. This step adds CORNERS + NEARBY_PROJECTIONS
// + PLANE_GRIDS on top of upstream's PLANES + RIDGES + RIDGE_LINES.
import { renderRoofGraph } from
  '../../../apps/lab-ui/src/utils/roof-graph-renderer.js';

export function render(ctx, data) {
  renderRoofGraph(ctx, data, 'boundary-signals');
}
