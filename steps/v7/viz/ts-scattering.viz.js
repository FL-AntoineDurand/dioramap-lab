// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: ts-scattering — λ3 / λ1, ∈ [0,1]. High = noise / 3D blob.
import { renderScalarGradientCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/scalar-gradient-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';

export function render(ctx, data) {
  renderScalarGradientCloud(ctx, data, {
    layerPath: TILE_SIGNALS_PATH,
    layerLabel: 'ts-scattering',
    layerId: 'tile-signals/ts-scattering',
    valueRange: [0, 1],
  });
}
