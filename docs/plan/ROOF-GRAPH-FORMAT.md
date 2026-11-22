# Roof-Graph TLV Format Specification

**Status:** locked schema, one per section, no versioning.
**Container:** Type-Length-Value (TLV) with section directory.
**Producer:** every v7-boundary-* step emits the SAME container kind
(`AK_OPAQUE_BLOB`, name `<step>-roof-graph`). Each step writes the
sections it owns + passes through every section it doesn't.
**Consumer:** downstream boundary steps read the sections they need;
the lab-ui visualizer reads any subset present.

## 0. Why TLV, and why no versioning

The bench-v2 boundary chain uses a single growing JS object passed
between steps. That works in JS (cheap object spread, no schema cost)
but forces v3 to either (a) emit one giant artifact per step that
duplicates everything upstream, or (b) split into many fine-grained
artifacts and re-read them all everywhere. Neither is good.

**TLV solves it:** one artifact per step, but its payload is a list of
typed sections. Step N starts from step N-1's blob, copies through
every section as bytes (zero parsing cost), then APPENDS its new
sections. Downstream steps decode only the sections they care about.
The viewer probes which sections are present and renders accordingly.

**No per-section version field.** A change to a section's schema
becomes a new section type. Old code reading an old file: still
works. Old code reading a new file: skips unknown type. New code
reading an old file: the old section just isn't there. This is the
"no versioning" the user asked for — each section is its own
immutable contract.

The CLAIM-VS-COMPATIBILITY thing is enforced by ID space: section
type IDs are assigned once in this doc and never reused. If we need
to evolve `CORNER`, we add `CORNER_V2` with a new ID. Same wire
format; new type. Old `CORNER` keeps working forever.

## 1. File layout

All multi-byte integers are little-endian. All structs are 8-byte
aligned within sections.

### 1.1 File header (32 bytes)

```
offset  size  field
   0     8    magic        = "RGRAPH\0\0" (0x52 47 52 41 50 48 00 00)
   8     4    flags        = reserved, must be 0
  12     4    n_sections   = u32, count of directory entries that follow
  16    16    reserved     = zeros
```

### 1.2 Section directory (24 bytes per entry)

Immediately after the header. n_sections entries.

```
offset  size  field
   0     4    type         = u32, see §3
   4     4    flags        = reserved, must be 0
   8     8    offset       = u64, byte offset from file start to section payload
  16     8    length       = u64, byte length of section payload
```

The directory is the ONLY way to find a section. Sections may appear in
ANY order in the payload area; the directory is the ground truth. A
reader iterates the directory and dispatches on `type`.

A section type appears AT MOST ONCE in a single file. Duplicate
section types are a write bug; readers MUST reject (return error).

### 1.3 Payload area

Sections back-to-back, each starting at its directory `offset`,
8-byte aligned. Trailing pad to 8-byte boundary between sections.

End-of-file: no trailer.

## 2. Section payload conventions

Each section payload starts with an 8-byte fixed-stride **section
header** that gives the record-stride convention, then records.

```
offset  size  field
   0     4    n_records       = u32, number of records
   4     4    record_stride   = u32, fixed stride in bytes, OR 0 = variable
   8     ...  records
```

- `record_stride > 0`: records are back-to-back at offsets `8 + k*stride`.
- `record_stride == 0`: each record is preceded by an 8-byte length
  prefix `{u32 record_len, u32 _zero_pad}`, followed by `record_len`
  payload bytes, then a trailing pad to 8-byte boundary.

Within a record, all multi-byte fields are little-endian. Sub-arrays
(e.g. a corner's `planeIds`) use the same length-prefix convention
when they need to be variable-length: `u32 count, u32 _zero_pad,
count × element_stride bytes`.

Strings are UTF-8 with a `u32 byte_len, u32 _pad, byte_len bytes`
prefix, padded to 8-byte boundary.

Optional fields use a presence flag in the record (typically a
bitmask in the fixed-stride header portion). When absent, the bytes
that would hold the field are unspecified — readers MUST NOT touch
them when the presence bit is 0.

## 3. Section type ID space

| ID    | Name                  | Producer            | Stride | Notes |
|-------|-----------------------|---------------------|--------|-------|
| 0x01  | PLANES                | boundary-signals    | fixed  | core node table; mutated only via overlays |
| 0x02  | RIDGES                | boundary-signals    | fixed  | core ridge table |
| 0x03  | RIDGE_LINES           | boundary-signals    | fixed  | parametric line + per-side extents |
| 0x04  | NEARBY_PROJECTIONS    | boundary-signals    | var    | per-plane sub-array of projected points |
| 0x05  | PLANE_GRIDS           | boundary-signals    | var    | per-plane signal grids (group + nearby fields, balance) |
| 0x06  | CORNERS               | boundary-corners    | var    | unified pool; planeIds[], ridgeIds[], dirsByPlane, inline members[] |
| 0x07  | _reserved_            | —                   | —      | dropped 2026-05-19; was CORNER_MEMBERS — folded into CORNERS as inline members[] block (flag 0x40) |
| 0x08  | _reserved_            | —                   | —      | dropped 2026-05-11; was PLANE_ANCHORS (redundant projection of CORNERS+planeIds+dirsByPlane) |
| 0x09  | MR_POINTS_BY_PLANE    | boundary-regularize | var    | per-plane multi-return points (only consumer is regularize) |
| 0x0A  | EDGES                 | boundary-polygons   | var    | polygon-graph edges |
| 0x0B  | POLYGON_PATHS         | boundary-polygons   | var    | per-plane ordered cornerId sequence |
| 0x0C  | PERIMETER_TAGS        | boundary-classify   | var    | overlay onto EDGES (edgeId → tag + detail) |
| 0x0D  | NEW_ADJACENCIES       | boundary-classify   | fixed  | eave-shared and similar synthetic adjacencies |
| 0x0E  | DOF_TAGS              | boundary-regularize | fixed  | per-corner degree-of-freedom + axes |
| 0x0F  | NUDGE_TRAJECTORIES    | boundary-regularize | var    | per-corner R.1/R.3 trajectory points |
| 0x10  | RESIDUALS_OVERLAY     | boundary-regularize | fixed  | per-corner residual at end of regularize (viz) |

IDs 0x11–0x7F reserved for future sections. IDs ≥ 0x80 reserved for
producer-private extensions; readers MUST skip unknown IDs without
error (forward-compatibility lifeline).

## 4. Section schemas

For each section: fixed header bytes are aligned to give the OUTER
record an 8-byte stride. Inner sub-arrays use the variable-length
convention from §2.

### 4.1 PLANES (0x01) — fixed stride 128 bytes

One record per plane group. `id` is the plane group index assigned
by `v7-grow-refit-merge`'s `grm-owner`. Tombstoned planes from GRM
ARE included (rejected=false but all their post-signal data
zeroed); the reader is expected to filter on `nPts > 0` if it cares
about live planes only.

```
offset  size  field
   0     4    id              u32   plane group id
   4     2    flags           u16   bit 0: rejected (1 = ground/veg/water majority)
                                    bit 1: has_basis
                                    bit 2: has_class_stats
                                    bit 3: has_polygon_path  (set iff a POLYGON_PATHS
                                                              entry exists for this plane)
                                    bit 4: has_dof          (set iff DOF_TAGS contains
                                                              this plane's corners)
   6     2    tilt_deg        u16   acos(|n_y|) * 180/π, rounded to int
   8    24    centroid        f64×3 (cx, cy, cz)
  32    24    normal          f64×3 (nx, ny, nz) — unit-length post-GRM
  56     8    d               f64   plane equation offset (n·p + d = 0)
  64     4    n_pts           u32   group point count
  68     4    rejection_reason u32  0 = none, 1 = "gvw>50%", 2 = "class-filter",
                                    3 = "low-nPts", … (fixed enum, see §5)
  72     8    slug_id         u64   FNV-1a hash of plane slug string. Use it as
                                    a key; the slug *string* lives in PLANE_SLUGS
                                    (0x80, optional viz-only sub-section).
                                    Two planes with the same hash are degenerate;
                                    treat hash as opaque.
  80    24    basis_e1        f64×3  if has_basis = 0, ignore
 104    24    basis_e2        f64×3  if has_basis = 0, ignore
```

### 4.2 RIDGES (0x02) — fixed stride 96 bytes

One record per ridge.

```
offset  size  field
   0     4    id              u32  same as ridge index in RIDGE_LINES
   4     4    plane_a         u32  plane group id A
   8     4    plane_b         u32  plane group id B
  12     1    type            u8   1 = ridge, 2 = hip, 3 = valley, 4 = coplanar,
                                    5 = step, 6 = eave, 0 = unknown (see §5)
  13     3    _pad
  16    24    from            f64×3 clipped start point
  40    24    to              f64×3 clipped end point
  64     8    length          f64
  72     4    contact_count   u32  cross-plane contact pairs
  76     4    overlap_ratio   f32  [0,1]
  80    16    _pad
```

### 4.3 RIDGE_LINES (0x03) — fixed stride 88 bytes

One record per ridge, parallel to RIDGES (same id ordering, same length).
Stores the parametric line + per-side extents needed for corner
detection and ridge endpoint snapping.

```
offset  size  field
   0     4    ridge_id        u32  matches RIDGES.id
   4     4    flags           u32  bit 0: has_line  (0 = degenerate, skip)
   8    24    line_point      f64×3
  32    24    line_direction  f64×3 (unit)
  56     4    t_min_a         f32
  60     4    t_max_a         f32
  64     4    t_min_b         f32
  68     4    t_max_b         f32
  72    12    side_vec        f32×3 perpendicular within plane A
  84     1    side_sign_a     i8   −1, 0, +1
  85     1    side_sign_b     i8
  86     2    _pad
```

### 4.4 NEARBY_PROJECTIONS (0x04) — variable stride

Per-plane sub-array of nearby (non-group) points projected onto the plane.
Consumed by boundary-polygons + boundary-regularize for residual lookup.

Section header n_records = number of planes with non-empty projections.

Each record:

```
offset  size  field
   0     4    plane_id        u32
   4     4    n_items         u32
   8     ...  items           n_items × {f64×3 position; f32 signed_dist; u32 src_idx}
                              = 36 bytes per item, 8-aligned (4 bytes pad after each)
                              = effective stride 40 bytes per item
```

Total record length = 8 + n_items × 40, padded to 8-byte boundary.

### 4.5 PLANE_GRIDS (0x05) — variable stride

Per-plane signal grids. Heavy. Most steps DON'T need this — only
boundary-corners (for OA/CoM/balance lookups) and some viz layers.
Step wrappers that don't need PLANE_GRIDS still pass it through as
opaque bytes.

**A plane may have more than one record.** v7-boundary-signals emits
GROUP_FIELD (with the optional BAL_FIELD trailer) as one record, then
NEARBY_FIELD as a second record with the same `plane_id` but its own
independent `n_cols/n_rows/g_min_u/g_min_v` (the nearby projections
generally have a different bbox than the plane's own points). Each
record is fully self-describing — consumers read a record, look at
the flag bits to know which field-set is inside, and use the header
dims for that field-set. Downstream consumers that only need group
filter to records where flag bit 0x01 is set.

Each record:

```
offset  size  field
   0     4    plane_id        u32
   4     4    flags           u32  bit 0: has_group_field
                                    bit 1: has_nearby_field
                                    bit 2: has_bal_field
   8     4    n_cols          u32
  12     4    n_rows          u32
  16     4    cell_size       f32  metres
  20     4    g_min_u         f32  (BUILDING-LOCAL frame)
  24     4    g_min_v         f32  (BUILDING-LOCAL frame)
  28     4    _pad
  32    24    eq_normal       f64×3  (cached plane normal)
  56     8    eq_d            f64    (BUILDING-LOCAL frame)
  64    24    basis_e1        f64×3
  88    24    basis_e2        f64×3
 112    24    building_centroid f64×3   (cx, cy, cz — F64 to keep
                                          full tile-world precision)
 136     ... fields, in order, each present iff its bit is set:
              GROUP_FIELD:   10 × float32×nCells + 1 × uint16×nCells
                             (oa, oaSpan, comDist, ptCount(u16), outU,
                              outV, bestLeft, bestRight, nodeX, nodeY, nodeZ)
                             — total bytes = nCells × (10*4 + 2) = nCells × 42,
                                + 2 B re-align AFTER ptCount when nCells is
                                odd (so the next f32 array starts 4-aligned;
                                JS Float32Array view would throw otherwise),
                                then padded to 8-byte block boundary.
              NEARBY_FIELD:  same field layout as GROUP_FIELD, BUT with
                             its OWN n_cols/n_rows/g_min_u/g_min_v in the
                             record header. Emitted as a separate
                             PLANE_GRIDS record (see paragraph above).
                             A record never carries both group AND
                             nearby data — flag bits 0x01 and 0x02 are
                             mutually exclusive on a given record.
              BAL_FIELD:     1 × float32×nCells = nCells × 4 bytes.
                             Only emitted with the group record (its
                             grid matches GROUP_FIELD's dims by
                             construction in compute_balance_field).
```

All position-bearing fields in the header + per-cell arrays (g_min_u,
g_min_v, eq_d, nodeX/Y/Z) are in the **building-local** frame; the
header's `building_centroid` is the F64 offset that converts to
tile-world. Storing tile-world as F32 (any of the per-cell arrays) at
L93 coords (~289 550 m) would collapse to ~3 cm precision via mantissa
truncation — visible as a staircase artefact on heat-map cells. The
F64 centroid + F32 local-frame split keeps cell positions sub-mm
exact while paying the larger F64 cost only once per record.

For sanity: a 100×100 grid with both GROUP and NEARBY fields = 100×100×(46+46+4) = 960 KB per plane. Most planes are smaller but it can spike.

### 4.6 CORNERS (0x06) — variable stride

Unified corner pool from boundary-corners. Each corner is its own
variable-length record because `planeIds`, `ridgeIds`, and
`dirsByPlane` are variable-length.

Each record:

```
offset  size  field
   0     4    id              u32   pool id (assigned at pool creation)
   4     1    source          u8    primary identity — cached max-priority
                                     across source_per_plane[]; same enum:
                                     1 = ridge-intersection, 2 = ridge-end,
                                     3 = polygon-merge, 4 = concave, 5 = convex,
                                     6 = mr-group, 7 = mr-nearby, 8 = coplanar (see §5)
   5     1    flags           u8    bit 0: has_name
                                     bit 1: has_meta_blob
                                     bit 2: has_dirs_by_plane
                                     bit 3: has_dof
                                     bit 4: has_path  (set iff a NUDGE_TRAJECTORIES
                                                       entry exists for this id)
                                     bit 5: has_com_bisector_by_plane
                                     bit 6: has_members  (inline pool-merge
                                                          decomposition — see
                                                          tail block below)
   6     2    n_merged        u16   count of source anchors merged
   8    24    point           f64×3 (post-nudge if regularize ran)
  32     8    plane_ids       prefix {u32 count; u32 _pad}
                              count × u32 plane_id
                              count × u8  source_per_plane (parallel to
                                                            plane_ids — the
                                                            per-plane source
                                                            label; same enum
                                                            as the cached
                                                            global `source`)
                              padded to 8-aligned
  ...    8    ridge_ids       prefix {u32 count; u32 _pad; count × u32 ridge_id}
                              padded to 8-aligned
  ...   if has_name:
        8    name            UTF-8 string with len-prefix from §2
  ...   if has_dirs_by_plane:
        8    dirs_by_plane   prefix {u32 count; u32 _pad}
                             count × { u32 plane_id; u32 _pad;
                                        f32×3 dir_a; f32×3 dir_b }
                             = 32 bytes per entry
  ...   if has_com_bisector_by_plane:
        8    com_bisector_by_plane   prefix {u32 count; u32 _pad}
                             count × { u32 plane_id; f32×3 com_bisector }
                             = 16 bytes per entry (no inner _pad — f32 is
                             naturally 4-aligned, and 16-B stride keeps
                             successive entries 8-aligned via the count
                             prefix's own 8-B layout).
                             — produced by v7-boundary-signals for
                             concave/convex anchors only (RI corners
                             skip). Caches the CoM-direction blended
                             bisector that v2 boundary-polygons
                             recomputes in its D.4 phase (a single
                             outward unit vector per (corner, plane)
                             derived from groupField.outU/V minus
                             nearbyField.outU/V at the anchor's grid
                             cell). v3 v7-boundary-polygons reads it
                             directly; v7-boundary-regularize will
                             reuse it for nudge direction.
                             NOTE: cached at signal-emission time
                             using the original candidate position,
                             which can diverge slightly from v2's
                             D.4 value when v7-boundary-corners pool
                             merge shifts an anchor's position (≤
                             0.25 m). See PHASE-5-V7-PORT.md.
  ...   if has_meta_blob:
        8    meta_blob       UTF-8 JSON, len-prefix from §2
                             (small key/value bag — comDistNorm,
                             spanDeg, wallBbox, snappedToRidge, …)
  ...   if has_members:
        8    members         prefix {u32 count; u32 _pad}
                             count × 48-B member entry, layout:
                               off 0..23   f64×3 position (tile-world)
                               off 24      u8    source (§5.3)
                               off 25      u8    authority (1=full, 2=axial, 3=none)
                               off 26      u8    has_axis
                               off 27      u8    has_ridge_id
                               off 28..39  f32×3 axis (zero if !has_axis)
                               off 40..43  u32   ridge_id (0 if !has_ridge_id)
                               off 44..47  u32   plane_id (0xFFFFFFFF if unknown
                                                  — e.g. polygon-merge members
                                                  introduced by boundary-merge
                                                  from polygon-graph corners
                                                  with no plane context)
                             Block stride is a multiple of 8, so no extra pad
                             after the array — the next record starts immediately.
```

**Why members are inline** — there is no consumer that walks members
without also walking the parent corner, so the historical CORNER_MEMBERS
section (0x07) just forced readers to do two passes over the same data.
Folding it inline keeps the per-corner state in one place and gives each
member an explicit `plane_id`, which boundary-polygons D.3 NMS uses to
pick the per-plane anchor position (v2 `planeAnchors[g]` semantics).

**Why `source_per_plane[]` is parallel to `plane_ids[]`** — coplanar import
(see `v7-boundary-corners`'s coplanar-import pass) registers a pool entry
into a foreign plane with `source = SRC_COPLANAR` while keeping its
origin plane's `source = SRC_CONCAVE`/`CONVEX`/`RIDGE_*`. The same pool
entry has different source labels in different planes. boundary-polygons'
D.3 NMS uses the per-plane source for that plane's priority sort.

The cached global `source` field MUST equal max-priority of
`source_per_plane[]` (priority order in §5). It exists for convenience —
viz coloring, naming, debug — so consumers that don't care per-plane
don't have to walk the array. `v7p_entry_register_plane` maintains this
invariant on every plane-attach, with `assert` in debug builds.

Section 0x08 PLANE_ANCHORS was dropped 2026-05-11 (see §4.8) under the
assumption that per-plane source could be derived from the global source.
Coplanar-import (added later) made that assumption invalid; the
`source_per_plane[]` array here restores the missing per-plane semantic
without resurrecting the redundant 0x08 section.

### 4.7 _reserved_ (0x07)

Dropped 2026-05-19. The original CORNER_MEMBERS section duplicated a
per-corner walk pattern that every consumer already did against
CORNERS. Members were folded into CORNERS as an inline tail block
gated by flag bit 0x40 (`has_members`); see §4.6. Emitters MUST NOT
use ID 0x07 and readers MUST ignore it if encountered in legacy
artifacts.

### 4.8 _reserved_ (0x08)

Dropped 2026-05-11. The original PLANE_ANCHORS section duplicated
state already carried by CORNERS (`planeIds` + `dirsByPlane`) plus
`source ∈ {concave, convex}`. Per-plane iteration is a trivial pass
over CORNERS, so emitters MUST NOT use ID 0x08 and readers MUST
ignore it if encountered in legacy artifacts.

### 4.9 MR_POINTS_BY_PLANE (0x09) — variable stride

Per-plane multi-return points. **Not produced by any v3 step** —
v7-boundary-regularize computes the per-plane MR set from its own
upstream inputs (positions + return-numbers + owner). The section ID
is reserved against the case where we ever want to materialise the
MR cache for visualisation or out-of-process debug. Bench-v2 emitted
this section from boundary-corners and threaded it through every
downstream step; v3 drops the pass-through entirely.

```
offset  size  field
   0     4    plane_id        u32
   4     4    n_points        u32
   8     ...  points          n_points × f64×3 = 24 B each (8-aligned)
```

### 4.10 EDGES (0x0A) — variable stride

Polygon-graph edges from boundary-polygons. Edges connect pool
corners and carry plane-membership info.

Each record:

```
offset  size  field
   0     4    id              u32
   4     4    flags           u32  bit 0: has_meta_blob
                                    bit 1: has_perim_tag
                                    bit 2: has_perim_detail
   8     4    corner_a        u32
  12     4    corner_b        u32
  16    12    dir             f32×3
  28     4    length          f32
  32     8    plane_ids       prefix {u32 count; u32 _pad; count × u32}
                              8-aligned. Typically count = 1 or 2.
  ...   if has_meta_blob: string §2 (JSON)
```

NOTE on perimeter tagging: the SOURCE step boundary-polygons emits
`flags = 0` (no perim tags). boundary-classify writes a separate
PERIMETER_TAGS section keyed by edge id — readers reconstruct the
union. This split keeps boundary-polygons' edges immutable across
classify, so passes after classify just need to read both sections.

### 4.11 POLYGON_PATHS (0x0B) — variable stride

Per-plane ordered cornerId sequence forming the closed boundary
polygon. Effectively the sequence of EDGES traversed in one plane's
cycle, but stored as cornerIds for trivial decode (no edge lookup
needed for polygon rendering).

Each record:

```
offset  size  field
   0     4    plane_id        u32
   4     4    n_corners       u32
   8     ...  corners         n_corners × u32 (cornerId)
```

### 4.12 PERIMETER_TAGS (0x0C) — variable stride

Overlay applied by boundary-classify. Maps edge_id → perimeter tag
+ outward_xz + optional detail blob. Edges not present in this section
have no tag (treated as untagged by readers).

Each record:

```
offset  size  field
   0     4    edge_id         u32
   4     1    tag             u8   1 = perimeter, 2 = shared, 3 = overhang-top,
                                    4 = overhang-bottom, 5 = interior-shared,
                                    6 = wall, 7 = level-transition (see §5)
   5     1    flags           u8   bit 0: has_detail
                                    bit 1: has_outward_xz (since 2026-05-26)
   6     2    _pad
   8     8    if has_outward_xz: outward_xz   f32×2 (outward_x, outward_z)
   ...   ...  if has_detail (after outward_xz block, or at offset 8 if !has_outward_xz):
              detail          variable. Schema:
                              { u32 n_coverages; u32 _pad
                                n_coverages × { f32 t0; f32 t1; u8 type;
                                                u8 _pad[3];
                                                u32 other_plane_id;
                                                u32 other_edge_id }
                                                = 24 B per coverage
                                f32 covered_fraction; u32 _pad
                                u8 has_pip_overhang; u8 _pad×7
                                if has_pip_overhang: f32 coverage; u32 other_plane_id }
```

Total record is 8-aligned. Minimum record (tag only) = 8 B. With
outward_xz = 16 B. Outward_xz is the unit horizontal vector pointing
AWAY from the roof polygon's interior — set by boundary-classify on
perimeter / level-transition edges (and any other edge that went
through the candidate-loop). Wall-detect consumes this to know which
side a wall projects toward.

### 4.13 NEW_ADJACENCIES (0x0D) — fixed stride 24 bytes

Per-pair record. Each entry: a NEW plane-pair adjacency synthesized
by boundary-classify (e.g. eave-shared edges that connect two planes
not connected by a ridge). Read by boundary-regularize for extra
constraints.

```
offset  size  field
   0     4    plane_a         u32
   4     4    plane_b         u32
   8     1    type            u8    1 = eave-shared, 2 = ridge-adjacent (see §5)
   9     3    _pad
  12     4    edge_id_a       u32
  16     4    edge_id_b       u32
  20     4    _pad
```

CLARIFICATION on `newAdjacencies`: in bench-v2 boundary-classify
attaches a `newAdjacencies` array to the roof-graph object, NOT a
per-edge property. It's a list of plane-pair-level adjacencies
discovered AFTER ridge-line adjacency was already used. The two
edges referenced (`edge_id_a`, `edge_id_b`) are the participating
edges from the two planes — boundary-regularize uses these to
propagate corner constraints across them.

### 4.14 DOF_TAGS (0x0E) — fixed stride 64 bytes

Per-corner degree-of-freedom record. Written by boundary-regularize.

```
offset  size  field
   0     4    corner_id       u32
   4     1    dof             u8   0 = locked (3+ planes),
                                    1 = ridge-slide (2 planes),
                                    2 = free (1 plane)
   5     1    flags           u8   bit 0: has_axis_a
                                    bit 1: has_axis_b
   6     2    _pad
   8    12    axis_a          f32×3
  20    12    axis_b          f32×3
  32    32    _pad
```

When `dof == 0`, axes are unused (zero out). When `dof == 1`,
axis_a = ridge direction, axis_b unused. When `dof == 2`, axis_a
and axis_b together span the plane.

### 4.15 NUDGE_TRAJECTORIES (0x0F) — variable stride

Per-corner trajectory of positions visited during R.1 nudge
optimization. Viz-only (cool to plot, no consumer). One record per
corner that moved; absent corners didn't move.

```
offset  size  field
   0     4    corner_id       u32
   4     4    n_steps         u32
   8     ...  steps           n_steps × f64×3 = 24 B each
```

### 4.16 RESIDUALS_OVERLAY (0x10) — fixed stride 16 bytes

Per-corner residual after regularize. Viz-only. Producers that
don't compute residuals omit this section entirely.

```
offset  size  field
   0     4    corner_id       u32
   4     4    n_planes_used   u32
   8     4    rms             f32   weighted RMS of corner-to-plane distances
  12     4    max_abs         f32   max absolute distance
```

### 4.17 PLANE_CONTOURS (0x11) — variable stride

Per-plane grid-fill topology, emitted by v7-boundary-signals (added
2026-05-22). One record per plane that produced a usable contour.

Computation: density-weighted rasterise of group points →
3×3 Gaussian smooth × 2 → binarise @ 0.1·maxDensity → ICM morphology
(10 iters) → marching-squares → chain into closed loops. Cell size
chosen by an adaptive trial loop (0.5× to 2× point spacing in 0.1 steps;
finest length-stable pick wins). No shrink step — vertices are raw
marching-squares cell-edge midpoints, fine for rough topology.

Loaded by v7-boundary-polygons-2 as the deterministic topology source
(replaces the FP-sensitive balance-walk).

```
offset  size  field
   0     4    plane_id        u32
   4     4    flags           u32   0x01 has_grids, 0x02 has_contour,
                                    0x04 has_holes, 0x08 has_nearby_grids
   8     4    n_cols          u32
  12     4    n_rows          u32
  16     4    cell_size       f32   metres (adaptive pick)
  20     4    g_min_u         f32
  24     4    g_min_v         f32
  28     4    max_density     f32   pre-threshold max smoothed value
  32     4    threshold       f32   = max_density · threshold_ratio
  36     4    _pad            u32   reserved, must be 0

  if has_grids (0x01):
    n_cells·4   density   f32 × n_cells   (raw rasterised weight, own points)
    n_cells·4   smoothed  f32 × n_cells   (post-Gaussian; cast from f64)
    n_cells     binary    u8  × n_cells   (post-ICM; pad to 8B boundary)

  if has_nearby_grids (0x08):
    n_cells·4   density_nearby   f32 × n_cells   (raw rasterised weight, nearby/foreign points)
    n_cells·4   smoothed_nearby  f32 × n_cells   (post-Gaussian; cast from f64)

  if has_contour (0x02):
    4              n_outer   u32
    +4             _pad      u32
    +n_outer·8     outer     f32×2 × n_outer (u,v interleaved)

  if has_holes (0x04):
    4              n_holes   u32
    +4             _pad      u32
    repeat n_holes:
      4            n_pts     u32
      +4           _pad      u32
      +n_pts·8     pts       f32×2 × n_pts

  pad to 8-byte boundary
```

Block ordering: has_grids → has_nearby_grids → has_contour → has_holes.
Each block is optional and self-describing via its flag; decoders walk
in this order. Adding has_nearby_grids was backward-compatible: a v1
reader that doesn't know the flag simply finds `has_contour`/`has_holes`
counts where it expects them only if the flag bit is unset.

Notes:
- `smoothed` is computed in f64 internally (v2 uses Float64Array; v3
  uses double*) and cast to f32 on emit. v2/v3 byte-eq requires both
  sides cast at the same point — after the second Gaussian pass.
- `outer` is the longest closed loop (highest n_pts); `holes` contains
  remaining loops with n_pts ≥ 4 (matches `slice(1).filter(h>=4)`).
- Starting vertex of `outer` depends on segment-iteration order. v2 (JS
  string-keyed Map) and v3 (C int64-keyed hashmap) can differ in start
  index; consumers should be tolerant of cyclic rotation.

### 4.18 PLANE_VOIDS (0x12) — variable stride

Per-plane membership in `dv2-ghost-planes` voids (added 2026-05-24).
One record per plane that owns ≥1 void. Planes with no voids emit no
record (lookup defaults to empty set).

Voids are atoms identified by a building-local `void_id` assigned by
`v7-density-voids-2`. The `(plane_id → {void_id,…})` mapping is built
by `v7-grow-refit-merge`: each plane starts with `{}`, ghost-seeded
planes start with `{ghost_void_id}`, and merges union the sets (the
survivor inherits the absorbed plane's voids). Topology re-emits the
data here so the per-bldg roof-graph carries the full mapping
self-contained.

Hull geometry per void lives in `dv2-ghost-planes` and is fetched by
boundary-signals (and later consumers) keyed by `void_id`.

```
offset  size  field
   0     4    plane_id        u32
   4     4    n_void_ids      u32
   8     n·4  void_ids        u32 × n_void_ids

  pad to 8-byte boundary
```

Notes:
- `plane_id` is grm-seed-index, matching the `id` in the PLANES
  section (which is grm-seed-index too).
- `void_id` is dv2's per-bldg id; join to `dv2-ghost-planes[void_id]`
  to recover hull + plane equation + RANSAC inliers.
- Records sorted by `plane_id` ascending for stable diffing.

## 5. Enum tables

Numeric IDs are FROZEN. New values append; old values never change
meaning.

### 5.1 Plane rejection reason

| ID | Name |
|----|------|
| 0  | none |
| 1  | gvw>50%               (ground+veg+water majority) |
| 2  | class-filter          (almost no building points) |
| 3  | low-nPts              (< 16 points) |
| 4  | tombstoned            (merged away in GRM) |

### 5.2 Ridge type

| ID | Name |
|----|------|
| 0  | unknown |
| 1  | ridge   (peak between two slopes) |
| 2  | hip     (corner between two slopes meeting non-perpendicularly) |
| 3  | valley  (V between two slopes facing each other) |
| 4  | coplanar (near-coplanar seam, dihedral < 10°; degenerate crease line — excluded from make-shared + DOF. Formerly "flashing") |
| 5  | step     (roof–wall eave) |
| 6  | eave     (between roof and wall) |

### 5.3 Corner source

| ID | Name | Authority |
|----|------|-----------|
| 1 | ridge-intersection | full  |
| 2 | ridge-end          | axial |
| 3 | polygon-merge      | full  |
| 4 | concave            | none  |
| 5 | convex             | none  |
| 6 | mr-group           | full  |
| 7 | mr-nearby          | none  |
| 8 | coplanar           | full  |

### 5.4 Corner-member authority

| ID | Name | DOF |
|----|------|-----|
| 1 | full   | 3D fix |
| 2 | axial  | 1D fix along ridge axis |
| 3 | none   | unconstrained / advisory |

### 5.5 Edge perimeter tag

| ID | Name |
|----|------|
| 0  | untagged          (sentinel — not stored, absence = untagged) |
| 1  | perimeter         (open sky boundary) |
| 2  | shared            (between two planes, same building) |
| 3  | overhang-top      (wall-meeting-roof, top of wall) |
| 4  | overhang-bottom   (wall-meeting-roof, bottom of wall) |
| 5  | interior-shared   (shared but inside footprint) |
| 6  | wall              (wall-aligned vertical edge) |
| 7  | level-transition  (step in roof level) |

### 5.6 New-adjacency type

| ID | Name |
|----|------|
| 1  | eave-shared      (two roof planes connected via shared eave only) |
| 2  | ridge-adjacent   (legacy ridge adjacency re-recorded for completeness) |

## 6. Pass-through semantics

Each step's wrapper does:

1. Open the upstream `<prev-step>-roof-graph` artifact.
2. Open a new writer for `<this-step>-roof-graph`.
3. For every section in upstream's directory whose type ID this
   step does NOT modify: copy the bytes verbatim into the new
   writer, register a new directory entry pointing at the copy.
4. Compute this step's new sections.
5. Append them as new directory entries.
6. Finalize.

Pass-through is byte-for-byte. A reader downstream sees exactly
what the original producer wrote, even if the section's ID isn't
known to any intermediate step. That's how unknown-section forward
compat works: the wrapper passes through every type ID, including
ones it doesn't recognize.

A step MAY overwrite a section — if so, it omits the upstream's
copy from the pass-through and writes its own. Only one producer
should ever own a given section ID per the table in §3, but this
pattern is allowed for future "rewrite-everything" steps.

## 7. Producer/consumer matrix

| Section            | Sig | Cor | Pol | Mer | Cls | Reg |
|--------------------|-----|-----|-----|-----|-----|-----|
| 0x01 PLANES        |  W  |  R  |  R  |  R  |  R  |  R  |
| 0x02 RIDGES        |  W  |  R  |  R  |  R  |  R  |  R  |
| 0x03 RIDGE_LINES   |  W  |  R  |  R  |  R  |  R  |  R  |
| 0x04 NEARBY_PROJ   |  W  |     |  R  |     |     |  R  |
| 0x05 PLANE_GRIDS   |  W  |  R  |  R  |     |     |     |
| 0x06 CORNERS       |     |  W  |  R  |  R  |  R  |  RW |
| 0x07 _reserved_    |     |     |     |     |     |     |
| 0x08 _reserved_    |     |     |     |     |     |     |
| 0x09 MR_POINTS     |     |     |     |     |     |  R  |
| 0x0A EDGES         |     |     |  W  |  RW |  R  |  R  |
| 0x0B POLY_PATHS    |     |     |  W  |  RW |     |  RW |
| 0x0C PERIM_TAGS    |     |     |     |     |  W  |  R  |
| 0x0D NEW_ADJACENCY |     |     |     |  R  |  W  |  R  |
| 0x0E DOF_TAGS      |     |     |     |     |     |  W  |
| 0x0F NUDGE_TRAJ    |     |     |     |     |     |  W  |
| 0x10 RESID_OVERLAY |     |     |     |     |     |  W  |

W = produces, R = consumes, RW = mutates.

A step that mutates an upstream section (e.g. boundary-merge edits
EDGES) drops upstream's copy from passthrough and emits its own.
Only sections with RW for the current step do this; all-R sections
flow through unchanged.

## 8. Out of scope (NOT in this format)

- The `boundary-signal-2d` pure-viz artifact stays separate (heavy,
  viz-only, pulled into the lab-ui via its own `*-2d` decoder).
- Per-point arrays (`owner`, `point-plane-residual`, etc.) stay
  separate AK_POINT_COLUMN artifacts. They're tile-wide, not
  graph-shaped.
- `plane-equations` is captured by `PLANES.normal` + `PLANES.d` —
  no separate section.
- `point-plane-distance-threshold` is per-step, not per-graph; it
  lives on `grm-distance-threshold` and is read directly by
  consumers that need it.

## 9. Reader/writer reference

C-side: `v3/steps/v7/v7-roof-graph-format.{h,c}` — see Phase 3 for
the implementation contract. Reader API is dispatch-on-type-id with
typed accessors per section. Writer API is "begin, append records,
finalize" with a passthrough helper for the upstream loop.

JS-side: `v3/apps/lab-ui/src/utils/roof-graph-decoder.js` — see
Phase 4. Returns a `{sections, planes(), ridges(), corners(),
edges(), planeGrids(...)}` accessor object that lazy-decodes only
the sections the visualizer needs.

CLI inspect: `v7-roof-graph-inspect <file.bin>` (Phase 5) — pretty-
prints the directory + per-section summary for debugging.
