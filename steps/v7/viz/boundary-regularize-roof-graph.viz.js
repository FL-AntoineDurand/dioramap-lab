// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: boundary-regularize-roof-graph — section-aware roof-
// graph renderer. This step nudges corner positions (R.1/R.2) + drops
// collinear vertices (R.3); section topology matches the upstream
// boundary-classify snapshot (PLANES + RIDGES + CORNERS + EDGES +
// POLYGON_PATHS + PERIMETER_TAGS + NEW_ADJACENCIES + …). Sub-layers
// land under "Roof Graph / boundary-regularize / …" with the same
// toggles the upstream step had.
import { renderRoofGraph } from
  '../../../apps/lab-ui/src/utils/roof-graph-renderer.js';

export function render(ctx, data) {
  renderRoofGraph(ctx, data, 'boundary-regularize');
}
