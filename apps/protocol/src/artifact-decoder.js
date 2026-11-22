// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Artifact binary decoder — kind-dispatched.
//
// The server ships each artifact's raw bytes inside one WS binary frame, with
// a preceding ARTIFACT_HEADER carrying `{artifact, size, hash}`. The bytes'
// internal layout depends on the artifact's kind (from the pipeline CATALOG
// response, not the ARTIFACT_HEADER — the header has no `kind` field). This
// module owns the kind → layout mapping.
//
// Supported kinds today (§5 of UNIFIED-ARCHITECTURE.md):
//   7  AK_OPAQUE_JSON     raw UTF-8 JSON text (no DMAP wrapper in current steps)
//   8  AK_ARRAY_INLINE    [u32-le N][u32-le stride][items] — stride=0 means
//                         each item is preceded by a u32-le length prefix.
//   9  AK_ARRAY_BY_HASH   [u32-le N][u32-le 0][N × 32-byte hashes]
//   10 AK_POINT_COLUMN    raw typed-array bytes; dtype inferred by name
//   11 AK_RASTER_F32      raster_format.h: u32 cols, u32 rows, u32 n_channels, F32…
//   12 AK_IMAGE_JPEG      raw JPEG bytes

// t() — identifier-class string marker. See viewer-sdk/src/viewer.js for
// the rationale; Phase 2 (deferred) substitutes via Vite plugin.
const t = (s) => s;

// Kind enum. The string names ('OPAQUE_JSON' etc.) are internal — the wire
// protocol carries kinds as integers only, so an attacker watching DevTools
// sees numbers, not names. The names DO leak in the JS bundle's `strings`
// output today; t()-wrapping them lets Phase 2's Vite plugin tokenize the
// keys at build time. Phase 1 is a no-op (computed keys with `['X']`
// resolve to the same string at runtime; bundle output unchanged).
//
// Note: dtype values like 'f64'/'f32'/'u8' are NOT t()-wrapped — they're
// protocol-public format names (every binary protocol exposes its dtype
// names), not internal identifiers.
export const AK = {
  ['POINTCLOUD_SOA']: 0,
  ['PER_POINT_ARRAY']: 1,
  ['GEOMETRY_PRIMITIVES']: 2,
  ['RASTER_2D']: 3,
  ['POLYGONS_3D']: 4,
  ['PLANE_RECORDS']: 5,
  ['MASK_BITSET']: 6,
  ['OPAQUE_JSON']: 7,
  ['ARRAY_INLINE']: 8,
  ['ARRAY_BY_HASH']: 9,
  ['POINT_COLUMN']: 10,
  ['RASTER_F32']: 11,
  ['IMAGE_JPEG']: 12,
};

// Point-column dtype registry. The wire format for AK_POINT_COLUMN is a raw
// typed-array with no self-describing dtype — steps emit F32/F64/U8/U16 bytes
// and the consumer is expected to know. Until CATALOG_RESPONSE carries per-
// artifact dtype metadata, we mirror the known shipped producers here.
// Matches laz-extract-disc.c + compute-normals.c writes.
const POINT_COLUMN_SCHEMA = {
  ['lidar-positions']:       { dtype: 'f64', components: 3 },  // interleaved x,y,z
  ['lidar-normals']:         { dtype: 'f32', components: 3 },  // interleaved nx,ny,nz
  ['lidar-spacing']:         { dtype: 'f32', components: 1 },  // per-point median k-NN distance
  ['lidar-classifications']: { dtype: 'u8',  components: 1 },
  ['lidar-intensities']:     { dtype: 'u16', components: 1 },
  ['lidar-num-returns']:     { dtype: 'u8',  components: 1 },
  ['canopy-flags']:          { dtype: 'u8',  components: 1 },
  ['grass-flags']:           { dtype: 'u8',  components: 1 },
  // V7 raw LAZ optional columns + derived (loader.tcbd_parse +
  // v7-beam-incidence). Beam-directions is interleaved bx,by,bz unit
  // vectors — visualised by per-point-rgb-cloud with abs-vec3-f32.
  ['scan-angles']:           { dtype: 'f32', components: 1 },
  ['point-source-ids']:      { dtype: 'u16', components: 1 },
  ['return-numbers']:        { dtype: 'u8',  components: 1 },
  ['number-of-returns']:     { dtype: 'u8',  components: 1 },
  ['bi-directions']:       { dtype: 'f32', components: 3 },
  // Per-segment: 6 F32 (x1,y1,z1,x2,y2,z2) in L93 metres. Producer is
  // build-parcel-lines.c — the artifact kind is AK_POINT_COLUMN even though
  // segments aren't strictly per-point; the registry-based decoder is the
  // simplest route to a typed view.
  ['parcel-lines']:          { dtype: 'f32', components: 6 },
};

export function decodeArtifact(kind, name, buffer) {
  const bytes = buffer instanceof ArrayBuffer ? buffer : buffer.buffer;
  // Name-suffix routing for roof-graph TLV artifacts. Two wire shapes:
  //   (a) single TLV blob       (any single-blob kind)
  //   (b) ARRAY_INLINE of TLVs  (one record per building — the v7
  //                              per-bldg pattern)
  // For (b), parse the outer ARRAY_INLINE first; the .items list
  // contains one TLV per building, decoded on-demand via .building(i).
  if (typeof name === 'string' && name.endsWith('-roof-graph')) {
    if (kind === AK['ARRAY_INLINE']) {
      return decodeRoofGraphArray(bytes);
    }
    return decodeRoofGraphTLV(kind, bytes);
  }
  switch (kind) {
    case AK['OPAQUE_JSON']:
      return decodeOpaqueJson(bytes);
    case AK['ARRAY_INLINE']:
      return decodeArrayInline(bytes);
    case AK['ARRAY_BY_HASH']:
      return decodeArrayByHash(bytes);
    case AK['POINT_COLUMN']:
      return decodePointColumn(bytes, name);
    case AK['RASTER_F32']:
      return decodeRasterF32(bytes);
    case AK['IMAGE_JPEG']:
      return { kind, bytes };
    default:
      return { kind, bytes, raw: true };
  }
}

// Roof-graph TLV — name-routed; the consumer (lab-ui renderer) calls
// pickRoofGraph(decoded) which knows how to decode .bytes or .items[idx]
// via its statically-imported decodeRoofGraph. We deliberately keep this
// module decoder-agnostic so @dioramap/protocol doesn't import lab-ui
// internals.
function decodeRoofGraphTLV(kind, bytes) {
  return {
    kind,
    bytes,
    raw: true,
    isRoofGraph: true,
  };
}

// ARRAY_INLINE-of-TLV: outer container with one TLV blob per building.
// We parse the outer ARRAY_INLINE here (just slicing) so consumers
// (renderer's pickRoofGraph) can synchronously index into items[].
function decodeRoofGraphArray(bytes) {
  const outer = decodeArrayInline(bytes);
  return {
    kind: AK['ARRAY_INLINE'],
    bytes,
    raw: true,
    isRoofGraph: true,
    isRoofGraphArray: true,
    n: outer.n,
    items: outer.items,
  };
}

function decodeOpaqueJson(bytes) {
  const text = new TextDecoder('utf-8').decode(bytes);
  let json = null;
  try { json = JSON.parse(text); } catch { /* leave json null */ }
  return { kind: AK['OPAQUE_JSON'], text, json };
}

// Array-inline layout (§5.3, Phase 5.1d format — record bytes 8-byte aligned):
//   u32-le n
//   u32-le stride (0 → variable-length, see below)
//   if stride > 0 (fixed-stride):
//     items back-to-back at offsets 8 + stride*k. Caller chose the
//     stride; if records contain typed fields it must be a multiple of
//     their alignment requirement (typically 8 for any record holding a
//     double).
//   if stride == 0 (variable-length):
//     for each record:
//       u32-le record_len
//       u32-le _pad (zero — keeps payload 8-aligned)
//       record_len bytes (8-aligned)
//       trailing pad to next 8-byte boundary
function decodeArrayInline(bytes) {
  const dv = new DataView(bytes);
  const n = dv.getUint32(0, true);
  const stride = dv.getUint32(4, true);
  const items = [];
  let off = 8; // u32 n + u32 stride
  if (stride === 0) {
    for (let i = 0; i < n; i++) {
      if (off + 8 > bytes.byteLength) break;
      const len = dv.getUint32(off, true);
      // off+4 is the 4-byte zero pad — skip it.
      const payloadOff = off + 8;
      if (payloadOff + len > bytes.byteLength) break;
      items.push(new Uint8Array(bytes, payloadOff, len));
      // Advance past payload + trailing pad to next 8-aligned offset.
      off = payloadOff + ((len + 7) & ~7);
    }
  } else {
    for (let i = 0; i < n; i++) {
      items.push(new Uint8Array(bytes, off, stride));
      off += stride;
    }
  }
  // The copc-tile-urls step emits JSON blobs per item; try to parse each as
  // text. Consumer can ignore `items_json` if they only want raw bytes.
  const items_json = items.map((u8) => {
    try { return JSON.parse(new TextDecoder('utf-8').decode(u8)); }
    catch { return null; }
  });
  return { kind: AK['ARRAY_INLINE'], n, stride, items, items_json };
}

function decodeArrayByHash(bytes) {
  const dv = new DataView(bytes);
  const n = dv.getUint32(0, true);
  const hashes = [];
  let off = 8; // u32 n + u32 stride(0)
  for (let i = 0; i < n; i++) {
    if (off + 32 > bytes.byteLength) break;
    hashes.push(new Uint8Array(bytes, off, 32));
    off += 32;
  }
  return {
    kind: AK['ARRAY_BY_HASH'],
    n,
    hashes,
    hashes_hex: hashes.map(toHex),
  };
}

function decodePointColumn(bytes, name) {
  const schema = POINT_COLUMN_SCHEMA[name];
  if (!schema) {
    return { kind: AK['POINT_COLUMN'], bytes, unknown: true, name };
  }
  const { dtype, components } = schema;
  const elemSize = dtypeBytes(dtype);
  const n_elems = Math.floor(bytes.byteLength / elemSize);
  const n_items = Math.floor(n_elems / components);
  const typed = toTypedArray(dtype, bytes, 0, n_elems);
  return {
    kind: AK['POINT_COLUMN'],
    name,
    dtype,
    components,
    n_items,
    data: typed,
  };
}

function decodeRasterF32(bytes) {
  // Header: u32 cols, u32 rows, u32 n_channels (little-endian).
  const dv = new DataView(bytes);
  const cols = dv.getUint32(0, true);
  const rows = dv.getUint32(4, true);
  const n_channels = dv.getUint32(8, true);
  const HEADER = 12;
  const cellsPerChannel = cols * rows;
  const channels = [];
  for (let c = 0; c < n_channels; c++) {
    const byteOffset = HEADER + c * cellsPerChannel * 4;
    if (byteOffset + cellsPerChannel * 4 > bytes.byteLength) break;
    channels.push(new Float32Array(bytes, byteOffset, cellsPerChannel));
  }
  return {
    kind: AK['RASTER_F32'],
    cols,
    rows,
    n_channels,
    channels,
  };
}

function dtypeBytes(dt) {
  return { u8: 1, i8: 1, u16: 2, i16: 2, u32: 4, i32: 4, f32: 4, f64: 8 }[dt];
}

function toTypedArray(dt, buf, byteOffset, length) {
  switch (dt) {
    case 'u8':  return new Uint8Array(buf, byteOffset, length);
    case 'i8':  return new Int8Array(buf, byteOffset, length);
    case 'u16': return new Uint16Array(buf, byteOffset, length);
    case 'i16': return new Int16Array(buf, byteOffset, length);
    case 'u32': return new Uint32Array(buf, byteOffset, length);
    case 'i32': return new Int32Array(buf, byteOffset, length);
    case 'f32': return new Float32Array(buf, byteOffset, length);
    case 'f64': return new Float64Array(buf, byteOffset, length);
    default: throw new Error(`unknown dtype: ${dt}`);
  }
}

function toHex(u8) {
  let s = '';
  for (let i = 0; i < u8.length; i++) s += u8[i].toString(16).padStart(2, '0');
  return s;
}
