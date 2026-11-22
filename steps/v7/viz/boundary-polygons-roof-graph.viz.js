// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: boundary-polygons-roof-graph — section-aware
// roof-graph renderer. This step adds POLYGON_PATHS (per-plane
// closed cornerId cycle) + EDGES (deduped global graph with
// planeIds[]); everything else passes through unchanged from the
// boundary-corners snapshot.
import { renderRoofGraph } from
  '../../../apps/lab-ui/src/utils/roof-graph-renderer.js';

export function render(ctx, data) {
  renderRoofGraph(ctx, data, 'boundary-polygons');
}
