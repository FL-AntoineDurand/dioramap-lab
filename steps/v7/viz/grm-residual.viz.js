// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: grm-residual — per-point |signed-distance| to the
// assigned plane. F32 per-point gradient cloud. +Infinity for
// unassigned points; we treat those as not visible (clamp to dim
// grey via the helper's auto-range, then bury in the histogram tail).
//
// distThreshold is 0.12 m for v7 — most residuals should sit below
// 0.05 m on real roofs. The histogram's max bucket is the tell-tale
// for over-grown / poorly-fit planes.

import { renderScalarGradientCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/scalar-gradient-cloud.js';

export function render(ctx, data) {
  renderScalarGradientCloud(ctx, data, {
    valueRange: [0, 0.12],          // 0 → distThreshold band; outliers saturate red
    layerPath: ['Grow-Refit-Merge'],
    layerLabel: 'grm-residual (|signed-dist| m)',
    layerId: 'grm/residual',
    histogramScale: 'log',
  });
}
