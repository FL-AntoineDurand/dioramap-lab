// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: ts-eigen-l3 — smallest PCA eigenvalue per point (F32).
// = variance along the surface normal. Near zero on flat surfaces.
import { renderScalarGradientCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/scalar-gradient-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';

export function render(ctx, data) {
  renderScalarGradientCloud(ctx, data, {
    layerPath: TILE_SIGNALS_PATH,
    layerLabel: 'ts-eigen-l3',
    layerId: 'tile-signals/ts-eigen-l3',
    histogramScale: 'log',
  });
}
