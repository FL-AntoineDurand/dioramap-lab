// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: boundary-merge-roof-graph — section-aware roof-graph
// renderer. This step REPLACES CORNERS (adds `polygon-merge` source-id
// 3 entries from cross-plane edge splices), EDGES (rebuilt global edge
// list), and POLYGON_PATHS (per-plane cycles after splice). Everything
// else passes through from the boundary-polygons snapshot.
import { renderRoofGraph } from
  '../../../apps/lab-ui/src/utils/roof-graph-renderer.js';

export function render(ctx, data) {
  renderRoofGraph(ctx, data, 'boundary-merge');
}
