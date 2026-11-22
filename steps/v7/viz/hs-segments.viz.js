// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: hs-segments — Int16 per-point segment id from
// hemisphere-segment. Categorical hue per segment id (golden-angle
// palette). Unassigned points (-1) get a dim grey so they don't
// dominate the scene but stay locatable.
import { renderCategoricalCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/categorical-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';

const HEMI_PATH = ['Hemisphere Cascade'];

export function render(ctx, data) {
  // Use a different group from tile-signals so the visibility tree
  // separates the two phases logically.
  void TILE_SIGNALS_PATH;
  renderCategoricalCloud(ctx, data, {
    layerPath: HEMI_PATH,
    layerLabel: 'hs-segments',
    layerId: 'hemisphere-cascade/segments',
  });
}
