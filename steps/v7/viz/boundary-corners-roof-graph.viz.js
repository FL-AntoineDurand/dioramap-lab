// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: boundary-corners-roof-graph — section-aware
// roof-graph renderer. This step REPLACES CORNERS with the unified
// pool (RI + ridge-end + concave + convex + coplanar — wall-bbox
// concave + ridge-snapped anchors) and appends the inline members[]
// tail (flag 0x40 — was a separate CORNER_MEMBERS section before
// 2026-05-19; see ROOF-GRAPH-FORMAT.md §4.6).
import { renderRoofGraph } from
  '../../../apps/lab-ui/src/utils/roof-graph-renderer.js';

export function render(ctx, data) {
  renderRoofGraph(ctx, data, 'boundary-corners');
}
