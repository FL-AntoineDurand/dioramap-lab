// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: boundary-classify-roof-graph — section-aware roof-
// graph renderer. This step ADDS PERIMETER_TAGS (0x0C) + NEW_ADJACENCIES
// (0x0D) overlays on top of the boundary-merge snapshot; everything else
// passes through. The renderer's renderPerimeterTags() handles both new
// sections — sub-layers land under "Roof Graph / boundary-classify /
// Classify" with toggles for tagged-edges, outward XZ normals, and new
// adjacencies.
import { renderRoofGraph } from
  '../../../apps/lab-ui/src/utils/roof-graph-renderer.js';

export function render(ctx, data) {
  renderRoofGraph(ctx, data, 'boundary-classify');
}
