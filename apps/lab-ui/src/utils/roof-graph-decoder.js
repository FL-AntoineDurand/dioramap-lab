// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Roof-graph TLV decoder — JS counterpart to v7-roof-graph-format.{h,c}.
//
// Spec: v3/docs/plan/ROOF-GRAPH-FORMAT.md.
//
// Usage:
//   import { decodeRoofGraph, RG_SECT } from '../utils/roof-graph-decoder.js';
//   const rg = decodeRoofGraph(bytes);     // bytes = ArrayBuffer | Uint8Array
//   if (rg.has(RG_SECT.PLANES)) {
//     for (const p of rg.planes()) { /* p.id, p.normal, p.tilt_deg, … */ }
//   }
//
// Lazy-decoding: section accessors return iterators that walk the
// payload on demand. The decoder doesn't materialize all sections
// up-front — heavy ones like PLANE_GRIDS only get parsed when a viz
// asks for them.
//
// All numeric fields read directly off the underlying ArrayBuffer
// using DataView (handles non-aligned offsets safely). Float
// arrays returned as Float32Array / Float64Array typed-array views
// where alignment permits, else as plain Array<number>.

const MAGIC = 'RGRAPH\0\0';
const FILE_HEADER_BYTES   = 32;
const DIR_ENTRY_BYTES     = 24;
const SECTION_HEADER_BYTES = 8;

export const RG_SECT = Object.freeze({
  NONE:                  0x00,
  PLANES:                0x01,
  RIDGES:                0x02,
  RIDGE_LINES:           0x03,
  NEARBY_PROJECTIONS:    0x04,
  PLANE_GRIDS:           0x05,
  CORNERS:               0x06,
  _RESERVED_07:          0x07,  // dropped 2026-05-19 (was CORNER_MEMBERS — now inline in CORNERS via flag 0x40)
  _RESERVED_08:          0x08,  // dropped 2026-05-11 (was PLANE_ANCHORS)
  MR_POINTS_BY_PLANE:    0x09,
  EDGES:                 0x0A,
  POLYGON_PATHS:         0x0B,
  PERIMETER_TAGS:        0x0C,
  NEW_ADJACENCIES:       0x0D,
  DOF_TAGS:              0x0E,
  NUDGE_TRAJECTORIES:    0x0F,
  RESIDUALS_OVERLAY:     0x10,
  PLANE_CONTOURS:        0x11,
});

const SECT_NAME = Object.fromEntries(
  Object.entries(RG_SECT).map(([k, v]) => [v, k]),
);

// Plane / corner / edge flag constants (mirror v7-roof-graph-format.h).
export const RG_PLANE_FLAGS = Object.freeze({
  REJECTED:        0x0001,
  HAS_BASIS:       0x0002,
  HAS_CLASS_STATS: 0x0004,
  HAS_POLYGON:     0x0008,
  HAS_DOF:         0x0010,
});
// plane_type field values (v7-roof-graph-format.h rg_plane_type_t) — an exclusive
// classification, NOT flags.
export const RG_PLANE_TYPE = Object.freeze(['unclassified', 'roof', 'wall', 'ground']);
export const RG_CORNER_FLAGS = Object.freeze({
  HAS_NAME:                  0x01,
  HAS_META_BLOB:             0x02,
  HAS_DIRS_BY_PLANE:         0x04,
  HAS_DOF:                   0x08,
  HAS_PATH:                  0x10,
  HAS_COM_BISECTOR_BY_PLANE: 0x20,
  HAS_MEMBERS:               0x40,
});
export const RG_EDGE_FLAGS = Object.freeze({
  HAS_META_BLOB:    0x01,
  HAS_PERIM_TAG:    0x02,
  HAS_PERIM_DETAIL: 0x04,
});

// Enum tables (mirrors §5 of the spec).
export const RG_RIDGE_TYPE = ['unknown','ridge','hip','valley','flashing','step','eave'];
export const RG_CORNER_SOURCE = [
  null,
  'ridge-intersection', 'ridge-end', 'polygon-merge',
  'concave', 'convex', 'mr-group', 'mr-nearby', 'coplanar',
];
export const RG_CORNER_AUTHORITY = [null, 'full', 'axial', 'none'];
export const RG_PERIM_TAG = [
  'untagged', 'perimeter', 'shared', 'overhang-top', 'overhang-bottom',
  'interior-shared', 'wall', 'level-transition',
];
export const RG_NEW_ADJ_TYPE = [null, 'eave-shared', 'ridge-adjacent'];
export const RG_REJECTION_REASON = [
  'none', 'gvw>50%', 'class-filter', 'low-nPts', 'tombstoned',
];

// ── public API ──

export function decodeRoofGraph(input) {
  let buffer, byteOffset, byteLength;
  if (input instanceof ArrayBuffer) {
    buffer = input; byteOffset = 0; byteLength = input.byteLength;
  } else if (ArrayBuffer.isView(input)) {
    buffer = input.buffer; byteOffset = input.byteOffset; byteLength = input.byteLength;
  } else {
    throw new TypeError('decodeRoofGraph: input must be ArrayBuffer or typed-array view');
  }
  return new RoofGraphReader(buffer, byteOffset, byteLength);
}

// Convenience: pull one building's RoofGraphReader out of an
// already-decoded artifact, regardless of whether the artifact is
// a single TLV blob (sourced from a tile-wide producer) or an
// ARRAY_INLINE-of-TLV (the per-bldg pattern). Returns null when
// the building has no graph (e.g. tombstoned/empty).
//
//   const rg = pickRoofGraph(decodedArtifact, buildingIdx);
//   if (rg) renderRoofGraph(ctx, rg, snapshotName);
//
// Synchronous: decodeRoofGraph is a synchronous DataView walk, no I/O.
export function pickRoofGraph(decoded, buildingIdx = 0) {
  if (!decoded) return null;
  if (decoded.isRoofGraphArray) {
    const u8 = decoded.items?.[buildingIdx];
    if (!u8 || u8.byteLength === 0) return null;
    return decodeRoofGraph(u8);
  }
  if (decoded.isRoofGraph) {
    if (decoded.bytes) return decodeRoofGraph(decoded.bytes);
    return null;
  }
  if (decoded instanceof ArrayBuffer || ArrayBuffer.isView(decoded)) {
    return decodeRoofGraph(decoded);
  }
  return null;
}

class RoofGraphReader {
  constructor(buffer, byteOffset, byteLength) {
    this._buffer = buffer;
    this._off = byteOffset;
    this._len = byteLength;
    this._dv = new DataView(buffer, byteOffset, byteLength);

    // Validate magic.
    if (byteLength < FILE_HEADER_BYTES) {
      throw new Error('RoofGraph: file shorter than header');
    }
    const magic = new TextDecoder('latin1').decode(
      new Uint8Array(buffer, byteOffset, 8),
    );
    if (magic !== MAGIC) {
      throw new Error(`RoofGraph: bad magic ${JSON.stringify(magic)}`);
    }
    const flags = this._dv.getUint32(8, true);
    if (flags !== 0) throw new Error(`RoofGraph: nonzero flags ${flags}`);
    this.nSections = this._dv.getUint32(12, true);

    // Build directory index { type → {offset, length} }.
    this._dir = new Map();
    for (let i = 0; i < this.nSections; i++) {
      const e = FILE_HEADER_BYTES + i * DIR_ENTRY_BYTES;
      const type = this._dv.getUint32(e, true);
      const off  = readU64(this._dv, e + 8);
      const len  = readU64(this._dv, e + 16);
      if (off + len > byteLength) {
        throw new Error(`RoofGraph: section ${type} overflows file`);
      }
      if (this._dir.has(type)) {
        throw new Error(`RoofGraph: duplicate section type ${type}`);
      }
      this._dir.set(type, { offset: off, length: len });
    }
  }

  has(type) { return this._dir.has(type); }

  sectionTypes() { return Array.from(this._dir.keys()); }

  // Returns the section payload bytes (Uint8Array view) or null.
  rawSection(type) {
    const entry = this._dir.get(type);
    if (!entry) return null;
    return new Uint8Array(this._buffer, this._off + entry.offset, entry.length);
  }

  // Section header (n_records, record_stride). Returns null if not present.
  sectionHeader(type) {
    const entry = this._dir.get(type);
    if (!entry) return null;
    if (entry.length < SECTION_HEADER_BYTES) return null;
    const dv = new DataView(this._buffer, this._off + entry.offset, entry.length);
    return {
      nRecords:     dv.getUint32(0, true),
      recordStride: dv.getUint32(4, true),
    };
  }

  // Generic record iterator — yields { record: Uint8Array, dv: DataView }
  // for each record, where `dv` is positioned at the record's start
  // (with byteOffset referring to the global ArrayBuffer for safe
  // multi-byte reads regardless of alignment).
  *rawRecords(type) {
    const entry = this._dir.get(type);
    if (!entry) return;
    if (entry.length < SECTION_HEADER_BYTES) return;
    const baseOff = this._off + entry.offset;
    const dv = new DataView(this._buffer, baseOff, entry.length);
    const n      = dv.getUint32(0, true);
    const stride = dv.getUint32(4, true);
    let off = SECTION_HEADER_BYTES;
    for (let i = 0; i < n; i++) {
      let recLen, payloadOff;
      if (stride !== 0) {
        if (off + stride > entry.length) return;
        recLen     = stride;
        payloadOff = off;
        off += stride;
      } else {
        if (off + 8 > entry.length) return;
        recLen = dv.getUint32(off, true);
        off += 8;  // skip u32 len + u32 zero pad
        if (off + recLen > entry.length) return;
        payloadOff = off;
        off += alignUp8(recLen);
      }
      const record = new Uint8Array(this._buffer, baseOff + payloadOff, recLen);
      const recDV  = new DataView(this._buffer, baseOff + payloadOff, recLen);
      yield { record, dv: recDV };
    }
  }

  // ── typed accessors ──

  *planes() {
    for (const { dv } of this.rawRecords(RG_SECT.PLANES)) {
      yield decodePlane(dv);
    }
  }

  *ridges() {
    for (const { dv } of this.rawRecords(RG_SECT.RIDGES)) {
      yield decodeRidge(dv);
    }
  }

  *ridgeLines() {
    for (const { dv } of this.rawRecords(RG_SECT.RIDGE_LINES)) {
      yield decodeRidgeLine(dv);
    }
  }

  *corners() {
    for (const { dv, record } of this.rawRecords(RG_SECT.CORNERS)) {
      yield decodeCorner(dv, record);
    }
  }

  *edges() {
    for (const { dv, record } of this.rawRecords(RG_SECT.EDGES)) {
      yield decodeEdge(dv, record);
    }
  }

  *newAdjacencies() {
    for (const { dv } of this.rawRecords(RG_SECT.NEW_ADJACENCIES)) {
      yield {
        planeA:    dv.getUint32(0, true),
        planeB:    dv.getUint32(4, true),
        typeId:    dv.getUint8(8),
        type:      RG_NEW_ADJ_TYPE[dv.getUint8(8)] || null,
        edgeIdA:   dv.getUint32(12, true),
        edgeIdB:   dv.getUint32(16, true),
      };
    }
  }

  // PERIMETER_TAGS — variable-stride records. Each record carries the
  // edge_id + tag plus optional flag-gated payloads:
  //   bit 0x01 has_detail:     n_coverages array + covered_fraction + pip_overhang
  //   bit 0x02 has_outward_xz: f32×2 outward (outX, outZ)
  // Spec: ROOF-GRAPH-FORMAT.md §4.12.
  *perimeterTags() {
    for (const { dv, record } of this.rawRecords(RG_SECT.PERIMETER_TAGS)) {
      const edgeId = dv.getUint32(0, true);
      const tagId  = dv.getUint8(4);
      const flags  = dv.getUint8(5);
      let off = 8;
      // Horizontal outward vector. Spec field is still called
      // PERIMETER_TAGS.outward_xz for back-compat with the v2 name, but
      // the v3 wrapper writes (X_v3, Y_v3) horizontal components in
      // z-up — see v7-boundary-classify.c axis swap. We label the
      // components .x and .y here to match the v3 scene convention.
      let outward = null;
      if (flags & 0x02) {
        outward = { x: dv.getFloat32(off, true), y: dv.getFloat32(off + 4, true) };
        off += 8;
      }
      const out = {
        edgeId,
        tagId,
        tag: RG_PERIM_TAG[tagId] || null,
        outward,  // .x / .y horizontal pair (z-up); was outwardXZ pre-2026-05-26
      };
      if (flags & 0x01) {
        const nCov = dv.getUint32(off, true);
        off += 8;
        const coverages = [];
        for (let i = 0; i < nCov; i++) {
          coverages.push({
            t0:           dv.getFloat32(off + 0, true),
            t1:           dv.getFloat32(off + 4, true),
            typeId:       dv.getUint8(off + 8),
            type:         RG_PERIM_TAG[dv.getUint8(off + 8)] || null,
            otherPlaneId: dv.getUint32(off + 12, true),
            otherEdgeId:  dv.getUint32(off + 16, true),
          });
          off += 24;
        }
        out.coverages = coverages;
        out.coveredFraction = dv.getFloat32(off, true);
        off += 8;
        const hasPip = dv.getUint8(off);
        off += 8;
        if (hasPip) {
          out.pipOverhang = {
            coverage:     dv.getFloat32(off, true),
            otherPlaneId: dv.getUint32(off + 4, true),
          };
        }
        // Re-yield with detail fields; if !hasPip, .pipOverhang stays undefined.
      }
      // Avoid the unused warning if `record` ever gets dropped — keep
      // the read so failed records surface via underlying record length
      // mismatches in rawRecords().
      void record;
      yield out;
    }
  }

  *dofTags() {
    for (const { dv } of this.rawRecords(RG_SECT.DOF_TAGS)) {
      const flags = dv.getUint8(5);
      yield {
        cornerId: dv.getUint32(0, true),
        dof:      dv.getUint8(4),
        flags,
        axisA: (flags & 0x01) ? [dv.getFloat32(8, true), dv.getFloat32(12, true), dv.getFloat32(16, true)] : null,
        axisB: (flags & 0x02) ? [dv.getFloat32(20, true), dv.getFloat32(24, true), dv.getFloat32(28, true)] : null,
      };
    }
  }

  *residuals() {
    for (const { dv } of this.rawRecords(RG_SECT.RESIDUALS_OVERLAY)) {
      yield {
        cornerId:    dv.getUint32(0, true),
        nPlanesUsed: dv.getUint32(4, true),
        rms:         dv.getFloat32(8, true),
        maxAbs:      dv.getFloat32(12, true),
      };
    }
  }

  *polygonPaths() {
    for (const { dv, record } of this.rawRecords(RG_SECT.POLYGON_PATHS)) {
      const planeId = dv.getUint32(0, true);
      const n       = dv.getUint32(4, true);
      const corners = new Uint32Array(record.buffer, record.byteOffset + 8, n);
      yield { planeId, corners };
    }
  }

  *nearbyProjections() {
    for (const { dv, record } of this.rawRecords(RG_SECT.NEARBY_PROJECTIONS)) {
      const planeId = dv.getUint32(0, true);
      const n       = dv.getUint32(4, true);
      const items = [];
      let off = 8;
      for (let i = 0; i < n; i++) {
        items.push({
          position: [
            dv.getFloat64(off + 0, true),
            dv.getFloat64(off + 8, true),
            dv.getFloat64(off + 16, true),
          ],
          signedDist: dv.getFloat32(off + 24, true),
          srcIdx:     dv.getUint32(off + 28, true),
        });
        off += 40;
        if (off > record.byteLength) break;
      }
      yield { planeId, items };
    }
  }

  // PLANE_GRIDS — yields per-plane records with the field blocks
  // exposed as typed-array views (no copy). flag bits:
  //   0x01 GROUP_FIELD   → groupField.{oa, oaSpan, comDist, ptCount,
  //                          outU, outV, bestLeft, bestRight,
  //                          nodeX, nodeY, nodeZ}
  //   0x02 NEARBY_FIELD  → nearbyField (same shape as group; emitted as
  //                                       a SECOND record per plane_id
  //                                       with its own independent dims)
  //   0x04 BAL_FIELD     → balField
  // All Float32 arrays unless typed (ptCount is Uint16).
  *planeGrids() {
    for (const { dv, record } of this.rawRecords(RG_SECT.PLANE_GRIDS)) {
      const planeId   = dv.getUint32(0, true);
      const flags     = dv.getUint32(4, true);
      const nCols     = dv.getUint32(8, true);
      const nRows     = dv.getUint32(12, true);
      const cellSize  = dv.getFloat32(16, true);
      // g_min_u/v are BUILDING-LOCAL. The renderer adds the centroid
      // shift in F64 (see below).
      const gMinU     = dv.getFloat32(20, true);
      const gMinV     = dv.getFloat32(24, true);
      const normal    = [dv.getFloat64(32, true), dv.getFloat64(40, true), dv.getFloat64(48, true)];
      const d         = dv.getFloat64(56, true);                 // local
      const basisE1   = [dv.getFloat64(64, true), dv.getFloat64(72, true), dv.getFloat64(80, true)];
      const basisE2   = [dv.getFloat64(88, true), dv.getFloat64(96, true), dv.getFloat64(104, true)];
      // F64×3 building centroid (header was extended from 112 B → 136 B).
      const centroid  = [dv.getFloat64(112, true), dv.getFloat64(120, true), dv.getFloat64(128, true)];
      const nCells = nCols * nRows;
      const base = record.byteOffset;
      let off = 136;

      // Helper: build a typed-array view at the current offset; bump off.
      const fld = (n) => {
        const a = new Float32Array(record.buffer, base + off, n);
        off += n * 4;
        return a;
      };
      const fld16 = (n) => {
        const a = new Uint16Array(record.buffer, base + off, n);
        off += n * 2;
        return a;
      };

      // After the u16 ptCount array, the producer inserts 2 padding
      // bytes when nCells is odd to keep the next f32 array 4-aligned
      // (otherwise the Float32Array constructor here throws). Mirror
      // that on read.
      const padAfterPtCount = (nCells & 1) ? 2 : 0;

      let groupField = null;
      if (flags & 0x01) {
        // Layout per spec §4.5: oa, oaSpan, comDist, ptCount(u16),
        // outU, outV, bestLeft, bestRight, nodeX, nodeY, nodeZ.
        groupField = {
          oa:        fld(nCells),
          oaSpan:    fld(nCells),
          comDist:   fld(nCells),
          ptCount:   fld16(nCells),
        };
        off += padAfterPtCount;
        Object.assign(groupField, {
          outU:      fld(nCells),
          outV:      fld(nCells),
          bestLeft:  fld(nCells),
          bestRight: fld(nCells),
          nodeX:     fld(nCells),
          nodeY:     fld(nCells),
          nodeZ:     fld(nCells),
        });
        off = alignUp8(off);
      }
      let nearbyField = null;
      if (flags & 0x02) {
        // Same field layout as group; emitted on its own grid (own
        // n_cols/n_rows/g_min_u/g_min_v in the record header). Producer
        // writes it as a separate PLANE_GRIDS record per plane_id.
        nearbyField = {
          oa:        fld(nCells),
          oaSpan:    fld(nCells),
          comDist:   fld(nCells),
          ptCount:   fld16(nCells),
        };
        off += padAfterPtCount;
        Object.assign(nearbyField, {
          outU:      fld(nCells),
          outV:      fld(nCells),
          bestLeft:  fld(nCells),
          bestRight: fld(nCells),
          nodeX:     fld(nCells),
          nodeY:     fld(nCells),
          nodeZ:     fld(nCells),
        });
        off = alignUp8(off);
      }
      let balField = null;
      if (flags & 0x04) {
        balField = fld(nCells);
        off = alignUp8(off);
      }
      yield {
        planeId, flags, nCols, nRows, cellSize, gMinU, gMinV,
        normal, d, basisE1, basisE2, centroid,
        groupField, nearbyField, balField,
      };
    }
  }

  // PLANE_CONTOURS — per-plane grid-fill topology (§4.17). Each record
  // carries: 40-byte header, optional inspection grids
  // (density/smoothed f32, binary u8 padded to 8B), optional outer
  // closed loop (u,v interleaved f32), optional holes block (each hole
  // is u32 n_pts + u32 _pad + f32×2×n_pts). Flags: 0x01 has_grids,
  // 0x02 has_contour (outer), 0x04 has_holes. u,v are building-local
  // plane coordinates — caller transforms via the matching plane's
  // basisE1/E2 + centroid to reach world.
  *planeContours() {
    for (const { dv, record } of this.rawRecords(RG_SECT.PLANE_CONTOURS)) {
      const planeId    = dv.getUint32(0,  true);
      const flags      = dv.getUint32(4,  true);
      const nCols      = dv.getUint32(8,  true);
      const nRows      = dv.getUint32(12, true);
      const cellSize   = dv.getFloat32(16, true);
      const gMinU      = dv.getFloat32(20, true);
      const gMinV      = dv.getFloat32(24, true);
      const maxDensity = dv.getFloat32(28, true);
      const threshold  = dv.getFloat32(32, true);
      const base = record.byteOffset;
      let off = 40;
      const nCells = nCols * nRows;
      let density = null, smoothed = null, binary = null;
      if (flags & 0x01) {
        density  = new Float32Array(record.buffer, base + off, nCells); off += nCells * 4;
        smoothed = new Float32Array(record.buffer, base + off, nCells); off += nCells * 4;
        binary   = new Uint8Array  (record.buffer, base + off, nCells); off += nCells;
        off = (off + 7) & ~7;
      }
      // flag 0x08 — nearby (foreign-plane) density/smoothed grids. Block
      // sits between has_grids and has_contour per ROOF-GRAPH-FORMAT §4.17.
      let densityNearby = null, smoothedNearby = null;
      if (flags & 0x08) {
        densityNearby  = new Float32Array(record.buffer, base + off, nCells); off += nCells * 4;
        smoothedNearby = new Float32Array(record.buffer, base + off, nCells); off += nCells * 4;
      }
      let outer = null;
      if (flags & 0x02) {
        const nOuter = dv.getUint32(off, true); off += 8;
        outer = new Float32Array(record.buffer, base + off, nOuter * 2);
        off += nOuter * 2 * 4;
      }
      let holes = [];
      if (flags & 0x04) {
        const nHoles = dv.getUint32(off, true); off += 8;
        for (let h = 0; h < nHoles; h++) {
          const n = dv.getUint32(off, true); off += 8;
          holes.push(new Float32Array(record.buffer, base + off, n * 2));
          off += n * 2 * 4;
        }
      }
      yield {
        planeId, flags, nCols, nRows, cellSize, gMinU, gMinV,
        maxDensity, threshold,
        density, smoothed, binary,
        densityNearby, smoothedNearby,
        outer, holes,
      };
    }
  }

  *mrPointsByPlane() {
    for (const { dv } of this.rawRecords(RG_SECT.MR_POINTS_BY_PLANE)) {
      const planeId = dv.getUint32(0, true);
      const n       = dv.getUint32(4, true);
      const points = [];
      for (let i = 0; i < n; i++) {
        const o = 8 + i * 24;
        points.push([
          dv.getFloat64(o, true),
          dv.getFloat64(o + 8, true),
          dv.getFloat64(o + 16, true),
        ]);
      }
      yield { planeId, points };
    }
  }
}

// ── per-record decoders ──

function decodePlane(dv) {
  const flags = dv.getUint16(4, true);
  const hasBasis = (flags & RG_PLANE_FLAGS.HAS_BASIS) !== 0;
  return {
    id:               dv.getUint32(0, true),
    flags,
    rejected:         (flags & RG_PLANE_FLAGS.REJECTED) !== 0,
    tiltDeg:          dv.getUint16(6, true),
    centroid: [dv.getFloat64(8,  true), dv.getFloat64(16, true), dv.getFloat64(24, true)],
    normal:   [dv.getFloat64(32, true), dv.getFloat64(40, true), dv.getFloat64(48, true)],
    d:                dv.getFloat64(56, true),
    nPts:             dv.getUint32(64, true),
    rejectionReason:  RG_REJECTION_REASON[dv.getUint32(68, true)] || 'unknown',
    slugId:           readU64(dv, 72),
    basisE1: hasBasis ? [dv.getFloat64(80, true), dv.getFloat64(88, true), dv.getFloat64(96, true)] : null,
    basisE2: hasBasis ? [dv.getFloat64(104, true), dv.getFloat64(112, true), dv.getFloat64(120, true)] : null,
    // plane_type field at offset 128 (record grew 128->136). Guard for pre-field
    // records (recLen may be the old 128 stride): default to unclassified.
    planeType:        dv.byteLength > 128 ? dv.getUint8(128) : 0,
    planeTypeLabel:   RG_PLANE_TYPE[dv.byteLength > 128 ? dv.getUint8(128) : 0] || 'unclassified',
  };
}

function decodeRidge(dv) {
  return {
    id:           dv.getUint32(0, true),
    planeA:       dv.getUint32(4, true),
    planeB:       dv.getUint32(8, true),
    typeId:       dv.getUint8(12),
    type:         RG_RIDGE_TYPE[dv.getUint8(12)] || 'unknown',
    from:         [dv.getFloat64(16, true), dv.getFloat64(24, true), dv.getFloat64(32, true)],
    to:           [dv.getFloat64(40, true), dv.getFloat64(48, true), dv.getFloat64(56, true)],
    length:       dv.getFloat64(64, true),
    contactCount: dv.getUint32(72, true),
    overlapRatio: dv.getFloat32(76, true),
  };
}

function decodeRidgeLine(dv) {
  const flags = dv.getUint32(4, true);
  return {
    ridgeId:    dv.getUint32(0, true),
    flags,
    hasLine:    (flags & 0x01) !== 0,
    linePoint:  [dv.getFloat64(8, true), dv.getFloat64(16, true), dv.getFloat64(24, true)],
    lineDir:    [dv.getFloat64(32, true), dv.getFloat64(40, true), dv.getFloat64(48, true)],
    tMinA:      dv.getFloat32(56, true),
    tMaxA:      dv.getFloat32(60, true),
    tMinB:      dv.getFloat32(64, true),
    tMaxB:      dv.getFloat32(68, true),
    sideVec:    [dv.getFloat32(72, true), dv.getFloat32(76, true), dv.getFloat32(80, true)],
    sideSignA:  dv.getInt8(84),
    sideSignB:  dv.getInt8(85),
  };
}

function decodeCorner(dv, record) {
  const id       = dv.getUint32(0, true);
  const sourceId = dv.getUint8(4);
  const flags    = dv.getUint8(5);
  const nMerged  = dv.getUint16(6, true);
  const point    = [dv.getFloat64(8, true), dv.getFloat64(16, true), dv.getFloat64(24, true)];

  // planeIds + sourcePerPlane block: u32 count + u32 _pad + count×u32
  // (planeIds) + count×u8 (sourcePerPlane), padded to 8. The parallel
  // u8 array carries the per-plane source label (e.g. SRC_COPLANAR=8 for
  // an import). See ROOF-GRAPH-FORMAT.md §4.6.
  let off = 32;
  const planeCount = dv.getUint32(off, true); off += 8;
  const planeIds = [];
  for (let i = 0; i < planeCount; i++) { planeIds.push(dv.getUint32(off, true)); off += 4; }
  const sourcePerPlane = [];
  for (let i = 0; i < planeCount; i++) { sourcePerPlane.push(dv.getUint8(off)); off += 1; }
  off = alignUp8(off);

  const ridgeCount = dv.getUint32(off, true); off += 8;
  const ridgeIds = [];
  for (let i = 0; i < ridgeCount; i++) { ridgeIds.push(dv.getUint32(off, true)); off += 4; }
  off = alignUp8(off);

  let name = null;
  if (flags & RG_CORNER_FLAGS.HAS_NAME) {
    const sl = readPaddedString(dv, off);
    name = sl.value;
    off = sl.next;
  }

  let dirsByPlane = null;
  if (flags & RG_CORNER_FLAGS.HAS_DIRS_BY_PLANE) {
    const dCount = dv.getUint32(off, true); off += 8;
    dirsByPlane = [];
    for (let i = 0; i < dCount; i++) {
      const planeId = dv.getUint32(off, true); off += 8;
      const dirA = [dv.getFloat32(off, true), dv.getFloat32(off + 4, true), dv.getFloat32(off + 8, true)]; off += 12;
      const dirB = [dv.getFloat32(off, true), dv.getFloat32(off + 4, true), dv.getFloat32(off + 8, true)]; off += 12;
      dirsByPlane.push({ planeId, dirA, dirB });
    }
    off = alignUp8(off);
  }

  let comBisectorByPlane = null;
  if (flags & RG_CORNER_FLAGS.HAS_COM_BISECTOR_BY_PLANE) {
    const cCount = dv.getUint32(off, true); off += 8;
    comBisectorByPlane = [];
    // Entry stride 16 B = u32 plane_id (off 0..3) + f32×3 com_bisector
    // (off 4..15). NO _pad after plane_id — f32 is 4-aligned by nature,
    // so the 16 B stride keeps successive entries 8-aligned too.
    for (let i = 0; i < cCount; i++) {
      const planeId = dv.getUint32(off, true);
      const comBisector = [
        dv.getFloat32(off + 4, true),
        dv.getFloat32(off + 8, true),
        dv.getFloat32(off + 12, true),
      ];
      off += 16;
      comBisectorByPlane.push({ planeId, comBisector });
    }
  }

  let metaBlob = null;
  if (flags & RG_CORNER_FLAGS.HAS_META_BLOB) {
    const sl = readPaddedString(dv, off);
    try { metaBlob = JSON.parse(sl.value); } catch { metaBlob = sl.value; }
    off = sl.next;
  }

  // Inline pool-merge decomposition (§4.6, flag 0x40, since 2026-05-19).
  // 48-B per member; positions are tile-world (caller subtracts centroid
  // if it wants building-local).
  let members = null;
  if (flags & RG_CORNER_FLAGS.HAS_MEMBERS) {
    const mCount = dv.getUint32(off, true); off += 8;
    members = [];
    for (let i = 0; i < mCount; i++) {
      const position = [
        dv.getFloat64(off + 0,  true),
        dv.getFloat64(off + 8,  true),
        dv.getFloat64(off + 16, true),
      ];
      const memberSourceId = dv.getUint8(off + 24);
      const authorityId    = dv.getUint8(off + 25);
      const hasAxis        = dv.getUint8(off + 26) !== 0;
      const hasRidgeId     = dv.getUint8(off + 27) !== 0;
      const axis = hasAxis ? [
        dv.getFloat32(off + 28, true),
        dv.getFloat32(off + 32, true),
        dv.getFloat32(off + 36, true),
      ] : null;
      const ridgeId = hasRidgeId ? dv.getUint32(off + 40, true) : null;
      const planeId = dv.getUint32(off + 44, true);
      members.push({
        position,
        sourceId: memberSourceId,
        source: RG_CORNER_SOURCE[memberSourceId] || 'unknown',
        authorityId,
        authority: RG_CORNER_AUTHORITY[authorityId] || null,
        axis, ridgeId,
        // 0xFFFFFFFF marks "no plane context" — usually a polygon-merge
        // member that boundary-merge synthesised without a plane assignment.
        planeId: planeId === 0xFFFFFFFF ? null : planeId,
      });
      off += 48;
    }
  }

  return {
    id, sourceId, source: RG_CORNER_SOURCE[sourceId] || 'unknown',
    flags, nMerged, point,
    planeIds, sourcePerPlane, ridgeIds, name, dirsByPlane, comBisectorByPlane, metaBlob,
    members,
    hasDof:  (flags & RG_CORNER_FLAGS.HAS_DOF)  !== 0,
    hasPath: (flags & RG_CORNER_FLAGS.HAS_PATH) !== 0,
  };
}

function decodeEdge(dv, _record) {
  const id    = dv.getUint32(0, true);
  const flags = dv.getUint32(4, true);
  const cornerA = dv.getUint32(8, true);
  const cornerB = dv.getUint32(12, true);
  const dir = [dv.getFloat32(16, true), dv.getFloat32(20, true), dv.getFloat32(24, true)];
  const length = dv.getFloat32(28, true);
  let off = 32;
  const planeCount = dv.getUint32(off, true); off += 8;
  const planeIds = [];
  for (let i = 0; i < planeCount; i++) { planeIds.push(dv.getUint32(off, true)); off += 4; }
  off = alignUp8(off);
  let metaBlob = null;
  if (flags & RG_EDGE_FLAGS.HAS_META_BLOB) {
    const sl = readPaddedString(dv, off);
    try { metaBlob = JSON.parse(sl.value); } catch { metaBlob = sl.value; }
    off = sl.next;
  }
  return { id, flags, cornerA, cornerB, dir, length, planeIds, metaBlob };
}

// ── helpers ──

function readU64(dv, off) {
  // JS Number can't lossless-represent values > 2^53. Roof-graph file
  // sizes are well under that, but use BigInt for the wire then Number-
  // ize. If you need full 64-bit precision, switch to BigInt directly.
  const lo = dv.getUint32(off, true);
  const hi = dv.getUint32(off + 4, true);
  return hi * 0x1_0000_0000 + lo;
}

function alignUp8(n) { return (n + 7) & ~7; }

// Padded string convention from §2: u32 byte_len, u32 _pad, byte_len UTF-8
// bytes, padded to 8-byte boundary. Returns { value, next }.
function readPaddedString(dv, off) {
  const len = dv.getUint32(off, true);
  // off+4 is zero pad
  off += 8;
  const u8 = new Uint8Array(dv.buffer, dv.byteOffset + off, len);
  const value = new TextDecoder('utf-8').decode(u8);
  return { value, next: off + alignUp8(len) };
}
