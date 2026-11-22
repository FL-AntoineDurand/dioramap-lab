// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: cs-owner — per-point plane id from
// v7-normal-cascade-seed (Int16, -1 = unassigned). Categorical hue per
// plane id (golden-angle palette). Same shape as hs-segments;
// renderCategoricalCloud's selection-changed listener wires this up to
// cross-viz selection from hs-lambert-grid peak clicks.
//
// Each owner-emitting step in V7 (seed → grm → refit → …) gets its
// own uniquely-named artifact + viz file (`cs-owner`, `grm-owner`,
// `refit-owner`) so the visibility-tree groups don't collide and each
// viz can hardcode its label/path. Downstream steps wire `owner →
// <upstream-step>::<step's owner name>` (matches v2's `owner`
// accumulator pattern at the wiring level rather than the artifact-
// name level).

import { renderCategoricalCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/categorical-cloud.js';

export function render(ctx, data) {
  renderCategoricalCloud(ctx, data, {
    layerPath: ['Seed Planes'],
    layerLabel: 'cs-owner (plane id)',
    layerId: 'seed/owner',
  });
}
