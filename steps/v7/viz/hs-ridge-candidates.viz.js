// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: hs-ridge-candidates — Uint8 bitmask per point.
// hemisphere-segment uses bit 7 (0x80) to flag orphan points (those
// dropped from a Lambert peak / offset cluster / spatial CC because
// the surviving area was below `min_segment_area`). Lower bits get
// populated later by the boundary chain (boundary-corners,
// boundary-classify, etc.) — eave / dormer / gap / etc. categories.
// Until those steps are ported, the layer label leans on "orphan" to
// reflect what's actually populated; a follow-up will switch to
// dynamic labelling based on which bits are set in the data.
import { renderBitmaskOverlay } from
  '../../../apps/lab-ui/src/renderers/_helpers/bitmask-overlay.js';

const HEMI_PATH = ['Hemisphere Cascade'];

export function render(ctx, data) {
  renderBitmaskOverlay(ctx, data, {
    layerPath: HEMI_PATH,
    layerLabel: 'hs-ridge-candidates (orphan only)',
    layerId: 'hemisphere-cascade/hs-ridge-candidates',
    bitLabels: [
      'ridge-class 0', 'eave', 'dormer', 'bit 3',
      'bit 4', 'ground-prox eave', 'gap', 'orphan',
    ],
  });
}
