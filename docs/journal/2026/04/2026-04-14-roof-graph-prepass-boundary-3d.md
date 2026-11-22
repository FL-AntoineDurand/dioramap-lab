---
id: "2026-04-14-roof-graph-prepass-boundary-3d"
date: 2026-04-14
title: "Roof Graph Pre-Pass + Snapshot Viz in boundary-3d"
tags: [boundary-3d, roof-graph, topology, refactor, viz]
category: development
status: concluded
refs:
  - "2026-03-13-v7-master-plan"
  - "2026-03-31-boundary-next-gen-plan"
  - "2026-03-29-plane-geometry-approaches"
pipeline: v7
---

## What

Introduce a roof-graph pre-pass into `boundary-3d`, mirroring (and reusing)
the rich topology graph built by `boundary-edges`. The graph is the substrate
for upcoming cross-plane work: shared corners, edge confirmation, footprint
association, watertighting (Stages E–G of the V7 master plan).

## Changes

### New shared module `lib/pipeline/steps/boundary-edges/roof-graph.js`

Hosts topology code so both `boundary-edges` and `boundary-3d` can use it:

- `detectCorners(ridgeLines, threshold)` — moved verbatim from
  `boundary-edges/index.js`. Ridge-ridge intersections, deduped within
  threshold (a corner where 3+ ridges meet collapses to one node).
- `snapRidgesToCorners(ridgeLines, corners, maxDist)` — moved verbatim.
  Mutates `tMin/tMax` to extend ridges to nearby corners.
- `buildRoofGraph(K, groupEquations, groupPoints, positions, ridgeLines,
  corners, adjPairs, spacing)` — moved verbatim. Returns
  `{ planes, ridges, corners }` with bidirectional links
  (`plane.ridgeIds`/`neighborIds`/`cornerIds`, `ridge.cornerIds`,
  `corner.ridgeIds`/`planeIds`).
- `roofGraphToLayers(roofGraph, snapshotName, opts?)` — **new**. Turns a
  roof-graph object into an array of viewer layers under
  `category: 'Roof Graph'`, `subcategory: snapshotName`. Layers emitted:
  Ridges (lines, type-colored), Plane Centroids (spheres), Plane Labels,
  Corners (spheres, source-tinted), Corner Labels, Centroid↔Ridge Links,
  Corner↔Centroid Links. All `defaultVisible: false` to avoid clutter.

### `boundary-edges/index.js` (no version bump — pure refactor)

Removed local `buildRoofGraph` / `detectCorners` / `snapRidgesToCorners`,
imports from new module. Behavior identical.

### `boundary-3d.js` v64 → v65

- Imports `detectCorners`, `buildRoofGraph`, `roofGraphToLayers`.
- New param `cornerThreshold` (default 0.25 m).
- Pre-pass after `buildRidgeLines`:
  1. Fit `groupEquationsRG` (one weighted plane fit per group).
  2. `detectCorners(ridgeLines, cornerThreshold)`.
  3. `buildRoofGraph(...)` → `initialRoofGraph`.
  4. Emit `adjacency-graph` artifact (new write).
  5. `roofGraphToLayers(initialRoofGraph, 'initial', { ridgeTypeColors })`
     → spread into the `boundary-3d` artifact's `layers` array.
- Pre-pass is **read-only** w.r.t. `ridgeLines` (no
  `snapRidgesToCorners`), so per-plane synthesis downstream is bit-for-bit
  identical to v64. The only functional changes are: a new artifact and
  additional viz layers.

## Why this shape

The user wants graph snapshots over the lifetime of a step run. As future
refinement passes (cross-plane corner merge, ridge-endpoint snap, edge
confirmation, belief propagation) modify the graph, each pass should emit
its own named snapshot ('cross-plane-merged', 'confirmed', …) by calling
`roofGraphToLayers(g, name)` again. The viewer's subcategory grouping makes
all snapshots live side-by-side in the overlay panel — toggle one to
inspect that state, compare across snapshots to see what each pass changed.

A single `adjacency-graph` artifact is emitted (the latest authoritative
state). Snapshots are viz-only; no need for snapshot artifacts in the DB.

## Verification

Smoke imports of all three modules pass. `boundary-3d.meta` reports
`v65` with both `boundary-3d` and `adjacency-graph` in its writes. Behavior
on real test cases not yet rerun — next session.

## Next

Per the boundary-next-gen plan:

1. Wire the pre-pass output through to the per-plane synthesis: replace
   the per-plane `closestApproach` ridge-corner anchors with shared
   anchors from `initialRoofGraph.corners`, so both planes' polygons
   reference the same XYZ point (eliminates the duplicate-corner drift).
2. Cross-plane polygon-vertex merge as a post-pass; emit a second
   snapshot `'cross-plane-merged'` showing the new `polygon-merge`
   corners.
3. Phase C — cross-plane edge confirmation (ridge-line snap when both
   adjacent planes voted) → snapshot `'confirmed'`.
4. Edge typing on output polylines (`ridgeId`, `neighborPlaneId`,
   `sharedCornerIds`) for downstream Stage F/G consumers.
