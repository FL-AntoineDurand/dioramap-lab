---
id: "2026-05-26-boundary-classify-port"
date: 2026-05-26
title: "v7-boundary-classify ported to v3 (Phase 5.7f)"
tags: [v3-port, boundary-chain, classify, iso-equivalence, phase-5]
category: development
status: concluded
refs: [2026-05-25-v2-v3-equivalence-and-pool-cleanup]
pipeline: v7
---

## Context

Phase 5.7f was the last "easy" port in the v7 boundary chain — bench-v2's
`boundary-classify.js` (Phase F perimeter edge classification, extracted
from the legacy `boundary-3d` monolith). Deterministic geometric
predicates, no FP-iterative convergence, additive overlays on the
roof-graph TLV. Risk surface was low; that prediction held.

## What landed

- **`v3/steps/v7/v7-boundary-classify-impl.{h,c}`** — pure-compute port of the algorithm. 7 phases:
  - **A** id maps (plane / corner / ridge → idx)
  - **B** per-non-wall-plane winding + XZ centroid from polygon path
  - **C** edge geometry (3D + XZ dirs, midpoint, lengths)
  - **D** classify each edge: wall short-circuit → multi-plane interior-shared → ridge-shared corners → candidate-loop with 3D/XZ collinearity + coverage union (shared > overhang-top > overhang-bottom > perimeter)
  - **E** dedup synthesized eave-shared adjacencies vs existing ridges (packed `(lo<<32|hi)` u64 keys)
  - **F** level-transition detection — perimeter roof-wall edges grouped by their wall plane; if 2+ distinct roof planes share a wall → retag as level-transition
  - **G** PIP fallback for overhangs — walk perimeter edges in 10 cm steps, probe 15 cm outward, check XZ containment in other planes' polygons
- **`v3/steps/v7/v7-boundary-classify.c`** — step wrapper. Reads per-bldg post-merge TLV, runs compute, emits PERIMETER_TAGS (0x0C) + NEW_ADJACENCIES (0x0D) overlays, passes through all other upstream sections.
- **`v3/steps/v7/v7-boundary-classify-only-bench.c`** — isolated bench. Ingests v2's `boundary-merge_0.roof-graph.json`, runs `v7_bclass_compute`, emits `perimeter-tags.json` / `new-adjacencies.json` / `summary.json` for diffing.
- **`v3/core/tests/v7_boundary_classify_test.c`** — 10 Stream-A unit tests (45 CHECK assertions): tag matrix (wall / multi-plane / ridge-shared / lone-perimeter / overhang-top / shared+adjacency / dedup-vs-ridge / level-transition), degenerate-edge handling, null-input rejection.
- **Iso-equivalence recipe** for boundary-classify in `tests/iso-equivalence.py`. Diffs tag counts, per-edge tag, outwardXZ presence + value, new-adjacency count.

## Spec change: PERIMETER_TAGS outward_xz extension

§4.12 of `ROOF-GRAPH-FORMAT.md` extended: flag bit 0x02 `has_outward_xz`
gates an optional 8-byte payload (f32×2 outward_x, outward_z) immediately
after the 8-byte fixed header. Forward-compatible — readers without the
bit see the original 8B header + skip the unknown trailer. Producer:
`v7-boundary-classify`. Consumer: `v7-wall-detect` (forthcoming).

Chose this over a separate section (e.g. RG_SECT_EDGE_OUTWARD_XZ) because:
- single producer (classify), single consumer (wall-detect)
- payload is small (8 B fixed)
- overlay semantics already match PERIMETER_TAGS (per-edge optional metadata)
- one fewer section = simpler reader code

## Validation result

Iso-equivalence on dahouet-bldgonly, v3 bench against v2's
`boundary-classify_0.roof-graph.json`:

```
=== boundary-classify ===
  → 5/5 pass, 0 drift
  [OK] tag counts:        v2={perim:26 shared:1 ovTop:53 ovBot:5 interior:17 wall:48}
                          v3=identical
  [OK] per-edge tag:      mismatches=0/150
  [OK] outwardXZ presence: mismatches=0
  [OK] outwardXZ values:   max |Δ|=2.93e-8   (F32 round-trip ceiling)
  [OK] new-adjacency count: v2=0 v3=0
```

The 2.93e-8 max-|Δ| on outwardXZ values is the floor — v3 stores as f32,
v2 stores as JSON-double; the round-trip is the entire divergence.

Stream-A test suite: **45/45 CHECK assertions pass**. Runtime <1s.

Total iso-equivalence sweep: **10/11 steps fully match v2** (was 9/10
before; boundary-corners and boundary-polygons drifts unchanged — known
pre-port issues, untouched by this work).

## Why the port was "easy"

- Algorithm is deterministic: no convergence loops, no random tie-breaks, no per-iteration cost minimization → no FP-sensitivity beyond storage cap.
- Output is additive: overlays on EDGES, no mutation of existing sections (PLANES / CORNERS untouched). Pass-through writer handles everything else.
- The "same-concept-N-encodings" anti-pattern ([[finding_same_concept_n_encodings]]) didn't surface here because there's no caching or per-position-provenance derivation; the algorithm reads inputs once and emits derived overlays once.
- Pre-existing v7-bench-v2-loaders already handle planes / corners / edges / polygon-paths (boundary-merge reuses); only had to add a tiny `load_classify_ridges` for the (id, planeA, planeB) projection.

## Open items (intentionally NOT done in this port)

- **Lab UI JS decoder** for PERIMETER_TAGS (0x0C) + NEW_ADJACENCIES (0x0D) — only matters once classify output needs visualization in the artifact-graph view. Currently nothing in v3 consumes this artifact through lab-ui. Wall-detect will need it; can land then.
- **Pipeline JSON** for `research-v7.json` — same: classify isn't part of any v3 prod pipeline run yet. The bench is enough to validate the algo; pipeline wiring lands together with wall-detect (5.7h).
- **wall-candidate-normals viz emission** — viz-only output, gated on emitVizArtifacts param. Skipped until lab-ui viz wires up.

## Files touched

```
v3/steps/v7/v7-boundary-classify-impl.h            new (146 LOC)
v3/steps/v7/v7-boundary-classify-impl.c            new (553 LOC)
v3/steps/v7/v7-boundary-classify.c                 new (407 LOC)
v3/steps/v7/v7-boundary-classify-only-bench.c     new (210 LOC)
v3/steps/v7/v7-boundary-classify.doc.json         new
v3/steps/v7/v7-steps.h                            +1 line  (extern)
v3/steps/v7/v7-steps.c                            +1 line  (register)
v3/core/CMakeLists.txt                            +6 lines (impl + step + bench compile)
v3/core/tests/CMakeLists.txt                      +1 word  (foreach item)
v3/core/tests/v7_boundary_classify_test.c         new (497 LOC, 10 tests, 45 checks)
v3/docs/plan/ROOF-GRAPH-FORMAT.md                 +14 lines (§4.12 outward_xz spec extension)
v3/docs/plan/PHASE-5-V7-PORT.md                   row 5.7f → done; status header bumped
v3/tests/iso-equivalence.py                       +60 lines (boundary-classify recipe)
```

## Next

- **5.7h `v7-wall-detect`** next. It's the smaller of the two remaining ports (~445 LOC v2 vs ~1481 for regularize) but more invasive on the data model (mutates PLANES + CORNERS + owner). First port that actually consumes our new outward_xz.
- **5.7g `v7-boundary-regularize`** last. Iterative coordinate-descent; will use stability-oracle harness (bp3 pattern), not byte-equivalence.
