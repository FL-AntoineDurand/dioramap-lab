// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: curvatures at K=16 — λ3 / (λ1+λ2+λ3), ∈ [0,1].
import { renderScalarGradientCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/scalar-gradient-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';

export function render(ctx, data) {
  renderScalarGradientCloud(ctx, data, {
    layerPath: TILE_SIGNALS_PATH,
    layerLabel: 'ts-curv-k16',
    layerId: 'tile-signals/ts-curv-k16',
    valueRange: [0, 1 / 3],
  });
}
