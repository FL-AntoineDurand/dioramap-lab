// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: ts-eigen-l1 — largest PCA eigenvalue per point (F32).
// Magnitude scales with neighbourhood size² — log-Y histogram is informative.
import { renderScalarGradientCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/scalar-gradient-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';

export function render(ctx, data) {
  renderScalarGradientCloud(ctx, data, {
    layerPath: TILE_SIGNALS_PATH,
    layerLabel: 'ts-eigen-l1',
    layerId: 'tile-signals/ts-eigen-l1',
    histogramScale: 'log',
  });
}
