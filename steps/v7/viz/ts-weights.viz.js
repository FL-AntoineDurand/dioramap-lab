// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: ts-weights — Voronoi cell area (m²) per point.
// Heavy-tailed distribution (high values at sparse outliers) → log-Y histogram.
import { renderScalarGradientCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/scalar-gradient-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';

export function render(ctx, data) {
  renderScalarGradientCloud(ctx, data, {
    layerPath: TILE_SIGNALS_PATH,
    layerLabel: 'ts-weights',
    layerId: 'tile-signals/ts-weights',
    histogramScale: 'log',
  });
}
