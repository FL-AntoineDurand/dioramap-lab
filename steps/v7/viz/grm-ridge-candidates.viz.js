// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: grm-ridge-candidates — Uint8 per-point mask, 1 if
// the point has any neighbour with a different `owner`. Highlights
// plane boundaries (ridge / valley / eave seams in the segmentation).

import { renderBitmaskOverlay } from
  '../../../apps/lab-ui/src/renderers/_helpers/bitmask-overlay.js';

export function render(ctx, data) {
  renderBitmaskOverlay(ctx, data, {
    layerPath: ['Grow-Refit-Merge'],
    layerLabel: 'grm-ridge-candidates',
    layerId: 'grm/ridge',
  });
}
