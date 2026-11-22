// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: ts-eigen-rel — density × angular isotropy, ∈ [0,1].
// Low = anisotropic neighbourhood (scan-line strip) or sparse density.
import { renderScalarGradientCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/scalar-gradient-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';

export function render(ctx, data) {
  renderScalarGradientCloud(ctx, data, {
    layerPath: TILE_SIGNALS_PATH,
    layerLabel: 'ts-eigen-rel',
    layerId: 'tile-signals/ts-eigen-rel',
    valueRange: [0, 1],
  });
}
