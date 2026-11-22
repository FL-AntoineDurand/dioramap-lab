// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Built-in line-segments renderer — placeholder for AK_GEOMETRY_PRIMITIVES.
//
// No v3 step ships this kind yet; once one does (roof-graph, boundary-3d
// ports), this module grows into a real 3D-primitive builder using the
// ctx.render3D layer API. For now, just describe what arrived.

export function render(ctx, data) {
  ctx.renderHTML(`
    <div class="render-placeholder">
      geometry-primitives render stub — ${data.bytes?.byteLength ?? 0} bytes
    </div>
  `);
}
