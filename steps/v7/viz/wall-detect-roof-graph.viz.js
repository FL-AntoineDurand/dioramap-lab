// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: wall-detect-roof-graph — section-aware roof-graph
// renderer. This step appends new wall planes (tilt=90°) into PLANES,
// 4 wall corners per wall into CORNERS, and 1 polygon path per wall
// into POLYGON_PATHS (replacing existing paths for merged walls).
// PERIMETER_TAGS + NEW_ADJACENCIES from upstream pass through, so the
// Classify sub-layers stay visible alongside the new wall geometry.
import { renderRoofGraph } from
  '../../../apps/lab-ui/src/utils/roof-graph-renderer.js';

export function render(ctx, data) {
  renderRoofGraph(ctx, data, 'wall-detect');
}
