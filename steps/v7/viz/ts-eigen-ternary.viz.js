// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: ts-eigen-ternary — Uint8×3N RGB packing of (ts-linearity,
// ts-planarity, ts-scattering). Already in [0,255] per the producer step.
import { renderPerPointRgbCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/per-point-rgb-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';

export function render(ctx, data) {
  renderPerPointRgbCloud(ctx, data, {
    inputType: 'uint8',
    layerPath: TILE_SIGNALS_PATH,
    layerLabel: 'ts-eigen-ternary',
    layerId: 'tile-signals/ts-eigen-ternary',
    componentLabels: ['ts-linearity', 'ts-planarity', 'ts-scattering'],
  });
}
