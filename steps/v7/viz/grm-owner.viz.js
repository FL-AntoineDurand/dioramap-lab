// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: grm-owner — final per-point plane id from
// v7-grow-refit-merge (Int16, -1 = unassigned). Same shape as
// cs-owner; renderCategoricalCloud's selection-changed listener
// wires this up to cross-viz selection from hs-lambert-grid
// peak clicks (peaks → segments → seeds → grm planes via
// source_seed_index in the grm-seed-planes record header).

import { renderCategoricalCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/categorical-cloud.js';

export function render(ctx, data) {
  renderCategoricalCloud(ctx, data, {
    layerPath: ['Grow-Refit-Merge'],
    layerLabel: 'grm-owner (plane id)',
    layerId: 'grm/owner',
  });
}
