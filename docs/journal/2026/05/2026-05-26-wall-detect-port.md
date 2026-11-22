---
id: "2026-05-26-wall-detect-port"
date: 2026-05-26
title: "v7-wall-detect ported to v3 (Phase 5.7h) — first PLANES/CORNERS mutator"
tags: [v3-port, wall-detect, beam-geometry, plane-mutation, iso-equivalence, phase-5]
category: development
status: concluded
refs: [2026-05-26-boundary-classify-port]
pipeline: v7
---

## Context

5.7h was the second-to-last v7 port. Wall-detect detects vertical wall
planes behind perimeter and level-transition edges using a beam-geometry
overhang lower bound, then either merges with an already-segmented wall
plane or emits a new one and grabs unassigned points onto it. It's the
first step in the v3 chain that **mutates PLANES and CORNERS** rather
than appending overlay sections, and the first real downstream consumer
of the `outward_xz` spec extension that landed with 5.7f boundary-classify.

## Pre-port audit findings (key calls)

1. **`plane.source` was dead in v2** — confirmed by grep: written nowhere
   in v2 main path except wall-detect.js itself, read by nobody. Dropped
   in v3 — no spec extension needed. (Same shape as a recognized
   anti-pattern: "produced just in case, but the case never came".)
2. **Owner stays I16** — v2 widens to I32 defensively but max plane count
   per realistic building is <100. Kept GRM's I16 stride; the wrapper
   asserts if any wall plane id exceeds INT16_MAX.
3. **`buildingGroundY` fail-loud** — v2 silently falls back to 0 if
   positions is empty (audit #275 silent-fallback class). v3 returns
   rc=-2 instead; empty positions is a pipeline-stage bug.
4. **6 derivable fields dropped from v2 plane**: `hull`, `area`, `slug`,
   `inlierIndices`, `inlierCount`, `azimuth`. polygonPath captures the
   geometry; owner.bin captures inliers.
5. **Sign-of-zero gates safe**: `cosAng` gated by `|cosAng| ≥ cos(10°)
   ≈ 0.985` before sign-use; `P_p > 0.05` gates the inward-side test.
6. **Determinism**: iterate `existingWalls` in plane-id order; iterate
   `wall-candidate edges` in perim_tags array order (parallel to input
   edges[]).
7. **v2 hull-proximity gate** in the merge phase: v2 checks each existing
   wall plane's hull for "near the edge midpoint". v3 doesn't carry
   hull (it's derivable from polygon-paths and not in scope), so v3
   matches v2's fallback branch (`else { near = true; }`) when ep.hull
   is empty — which is the case for every wall plane in scope.

## What landed

- **`v3/steps/v7/v7-wall-detect-impl.{h,c}`** — pure-compute port,
  6 phases per wall-candidate edge:
  - 0  Bootstrap (ground y, next ids, existing-walls filter)
  - 1  Beam-geometry overhang bound (back-project beam to yTop,
       δ_this = max(P_p, P_t) over witnesses below yTop-buffer)
  - 2  Existing-wall merge (signed-dot normal alignment, score by
       `|inwardDist|`, merge if within outward-tolerance + cap-buffer)
  - 3  Plane allocate or reuse (new vertical plane vs update existing)
  - 4  4 wall corners (project edge endpoints onto wall plane, drop to
       building-ground-y for bottoms)
  - 5  Point grab (owner==-1 + y range + xz bbox + perp-dist gate)
- **`v3/steps/v7/v7-wall-detect.c`** — step wrapper. Reads tile-wide
  lidar-positions / bi-directions, per-bldg roof-graph TLV / grm-owner /
  bldg-thresholds. Subsets positions to per-building local frame via
  per-building-records' gidx. Re-emits PLANES (passthrough + append
  walls), CORNERS (record-level passthrough + 4 per wall), POLYGON_PATHS
  (passthrough non-wall + replace/append per wall). Emits per-bldg
  wd-owner (I16, narrowed from I32 compute buffer).
- **`v3/steps/v7/v7-wall-detect-only-bench.c`** — isolated bench.
- **`v3/core/tests/v7_wall_detect_test.c`** — 8 Stream-A tests, 39 CHECK
  assertions: wall creation, existing-wall merge, non-perimeter tag
  ignored, missing-outwardXZ skipped, point-grab inclusion/exclusion,
  beam-bound delta calculation, null-input rejected, zero-edge no-work.
- **`v3/tests/iso-equivalence.py`** — wall-detect recipe.
- **`v3/pipelines/smoke/research-v7-tile-signals-real.json`** —
  wall-detect appended after boundary-classify.
- **`v3/steps/v7/viz/wall-detect-roof-graph.viz.js`** — 5-line delegate
  routing through the section-aware renderer (no new viz code needed;
  walls render via existing PLANES + CORNERS + POLYGON_PATHS paths).

## Validation result

**Iso-equivalence on dahouet-bldgonly: 8/8 PASS, 0 drift.**

```
=== wall-detect ===
  [OK] owner.bin byte-equiv:           16092 I32 points identical
  [OK] per-edge deltaSource:           mismatches=0
  [OK] per-edge mergedWith:            mismatches=0
  [OK] per-edge pointsGrabbed:         mismatches=0
  [OK] per-edge deltaMin (1 mm quantum): max |Δ|=4.675e-4
  [OK] walls created:                  v2=14 v3=14
  [OK] walls merged:                   v2=5  v3=5
  [OK] points grabbed:                 v2=30 v3=30
```

The 4.7e-4 max delta on deltaMin is exactly v2's `+deltaMin.toFixed(3)`
quantization step (rounded to mm in the debug-JSON output). Underlying
algorithm output is bit-equivalent within F32 precision.

**Stream-A**: 39/39 CHECK assertions pass.

**End-to-end smoke** (dioramap-processor batch): 17 steps, classify
210 ms + wall-detect 222 ms on dahouet, wall-detect-roof-graph 24.7 MB,
wd-owner 32 KB.

## What was harder than expected

- **Tile-wide ↔ per-bldg plumbing** — wall-detect needs `positions` and
  `bi-directions` from the tile but works on per-bldg roof-graph. v3
  has well-established mechanics for this (the gidx pattern from GRM)
  but the wrapper had to fuse three artifact-shape regimes (tile-wide
  AK_POINT_COLUMN, per-bldg ARRAY_INLINE-of-TLV, per-bldg
  ARRAY_INLINE-of-I16).
- **TLV mutation pattern** — first time we needed to ADD records into
  PLANES (not just overlays). Pattern used: re-emit the section from
  scratch (`rg_writer_section_begin` → copy upstream records verbatim
  → append synthesized records → `rg_writer_section_end`). The
  passthrough-via-byte-copy trick (passing `(rec_bytes, rec_len)` from
  reader straight to writer) means we never re-parsed the variable-
  stride CORNERS records, just appended fresh wall corner records
  alongside.

## Open items intentionally NOT done

- **No `wall-polygons` viz artifact** — v2 emits a separate edges
  artifact with the same 4-edge wall outlines that POLYGON_PATHS
  already provides. Redundant.
- **No `wall-debug` viz artifact** — per-edge diagnostics; useful for
  debugging but not viz-blocking. Available via iso bench instead.
- **No wall-specific render style** — walls render in the same color
  scheme as roof planes (per-id hue). A "Walls" sub-layer with
  distinctive style (e.g. brown) is a polish nice-to-have, not required.

## Files

```
v3/steps/v7/v7-wall-detect-impl.h                  new  (179 LOC)
v3/steps/v7/v7-wall-detect-impl.c                  new  (370 LOC)
v3/steps/v7/v7-wall-detect.c                       new  (530 LOC)
v3/steps/v7/v7-wall-detect-only-bench.c            new  (218 LOC)
v3/steps/v7/viz/wall-detect-roof-graph.viz.js      new  (13 LOC)
v3/steps/v7/v7-steps.{h,c}                         +2 lines (register)
v3/core/CMakeLists.txt                             +6 lines (lib + bench)
v3/core/tests/CMakeLists.txt                       +1 word  (foreach item)
v3/core/tests/v7_wall_detect_test.c                new  (250 LOC, 8 tests, 39 checks)
v3/pipelines/smoke/research-v7-tile-signals-real.json  +13 lines
v3/docs/plan/PHASE-5-V7-PORT.md                    row 5.7h → done, status hdr bumped
v3/tests/iso-equivalence.py                        +60 lines (wall-detect recipe)
```

## Next

**5.7g `v7-boundary-regularize`** is the only remaining v7 port (terminal
artifact). It's iterative coordinate-descent on free corner positions
with ≥6 saturated cost terms; FP-sensitive convergence means byte-
equivalence won't be achievable. Validation strategy will be the
stability-oracle pattern (σ=1mm perturbation, ≥99/100 match), the
same approach that landed for bp3 in May.
