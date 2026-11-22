// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-building-records parser — shared by step-visualizers that consume an
// AK_ARRAY_INLINE artifact paired with the per-building-records sibling
// (one record per building, indices into lidar-positions).
//
// Header layout (matches v3/steps/buildings/extract-buildings.c):
//   off 0   u32  schema = 1
//   off 4   u32  n_verts
//   off 8   u32  n_pts
//   ...     56 bytes additional header (centroid, bbox, ...) — ignored here
//   off 68          footprint vertices: f64×3 × n_verts
//   off 68 + 24·v   global-point indices: u32 × n_pts (into lidar-positions)

export function parseBuildingHeader(u8) {
  if (!u8 || u8.byteLength < 68) return null;
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const schema = dv.getUint32(0, true);
  if (schema !== 1) return null;
  const n_verts = dv.getUint32(4, true);
  const n_pts = dv.getUint32(8, true);
  const idxOff = 68 + n_verts * 24;
  if (idxOff + n_pts * 4 > u8.byteLength) return null;
  return {
    schema,
    n_verts,
    n_pts,
    globalIndices: new Uint32Array(u8.buffer, u8.byteOffset + idxOff, n_pts),
  };
}
