// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: boundary-topology-roof-graph — section-aware
// roof-graph renderer (PLANES / RIDGES / RIDGE_LINES sections only;
// CORNERS / EDGES / GRIDS appear later in the chain).
import { renderRoofGraph } from
  '../../../apps/lab-ui/src/utils/roof-graph-renderer.js';

export function render(ctx, data) {
  renderRoofGraph(ctx, data, 'boundary-topology');
}
