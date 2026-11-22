// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: ts-normals — F32×3N raw computed normals.
// Coloured by |nx|, |ny|, |nz| → R, G, B (the standard normal-map convention).
import { renderPerPointRgbCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/per-point-rgb-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';

export function render(ctx, data) {
  renderPerPointRgbCloud(ctx, data, {
    inputType: 'abs-vec3-f32',
    layerPath: TILE_SIGNALS_PATH,
    layerLabel: 'ts-normals',
    layerId: 'tile-signals/ts-normals',
    componentLabels: ['|nx|', '|ny|', '|nz|'],
  });
}
