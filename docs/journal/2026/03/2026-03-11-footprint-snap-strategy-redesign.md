---
id: "2026-03-11-footprint-snap-strategy-redesign"
date: 2026-03-11
title: "Footprint snap strategy redesign: from per-edge projection to detect-then-match"
tags: [snap-footprint, detect-boundary, hough, ransac, edge-detection, co-registration, design]
category: design
status: proposed
refs:
  - "2026-03-11-detect-boundary-bits-5-6-7"
  - "2026-03-09-boundary-co-registration"
  - "2026-03-10-physics-multireturn-edge-classification"
pipeline: exploration
---

## Motivation

BDTOPO footprints are approximate polygons drawn by IGN cartographers. Global
co-registration (translating/rotating the whole polygon) corrects systematic
offset but cannot fix per-edge shape errors — an edge drawn 1m too far north,
a missing indent, a corner in the wrong place.

The snap-footprint step was introduced to fix individual edges by snapping
each one to nearby LiDAR-detected boundary candidates. Over two sessions of
development, its limitations became clear and led to a fundamentally different
approach.

## Session 1 — detect-boundary enrichment (bits 5-7)

### Starting point

The detect-boundary step had 5 bits (0-4): gap, eave, dormer, obstacle,
multi-return. Snap-footprint only consumed bit 5 (ground-proximity eave),
which was a simple criterion: building points with ≥2 ground neighbors in
k-NN.

### Changes made

1. **Class 1 (unassigned) inclusion**: IGN's "unassigned" points (class 1,
   ~8% of real test case points) often contain building returns. Added
   `isBuildingOrUnassignedClass()` to include them in boundary detection.
   Critical fix: density must still be computed from class 6/67 only — class 1
   is scattered and dilutes density from 12.8 to 4.7 pts/m², breaking gap
   detection entirely.

2. **Phase 0c merged into Phase 0a**: ground-proximity eave detection (bit 5)
   was a separate pass with its own k-NN. Merged into Phase 0a's existing
   k-NN loop — `testPoint()` now returns `{ gap, groundNbCount }`, no extra
   spatial query needed. Bit 5 is mutually exclusive with bit 1 (strict eave
   takes priority).

3. **Bit 6 — Wall-return (64)**: not-first-return points (returnNumber > 1)
   near first-return multi-return points in XZ. Physics: beam-split at
   building edge → first return hits roof, second+ continues to wall/ground.
   Their XZ position marks the building edge precisely. Weight 2.0.

4. **Bit 7 — MR-noise (128)**: not-last-return points that failed Phase 0b
   grid-clustering + connected-components filtering. Isolated multi-return
   points that are noise, not coherent edges.

5. **Documentation**: created `detect-boundary.md` alongside implementation
   with full pseudo code, bit table, exclusion rules, UI rendering,
   consumer table, and parameters.

### Result on dahouet (24-edge building)

```
density=12.8 pts/m², gap=860 (eave=651, dormer=63, obstacle=146),
multi-return=679, mr-noise=158, wall-return=247,
ground-prox=705, total=2113 candidates
```

## Session 2 — snap-footprint v5 (per-edge rotation + elongation)

### What snap-footprint v4 did

For each BDTOPO footprint edge:
1. Project nearby boundary candidates onto edge's normal axis
2. Histogram the perpendicular offsets (bin width 0.15m)
3. Find the dominant peak → snap offset
4. Shift the edge by that offset
5. Reconstruct polygon by intersecting consecutive shifted edge lines

This was translation-only: each edge could move perpendicular to itself but
could not rotate or change length.

### Changes made (v5)

1. **All boundary bits**: changed from bit 5 only to all bits except dormer
   (bit 2) and obstacle (bit 3). Using bits 0,1,4,5,6,7 increased candidates
   from 705 to 2113 for dahouet.

2. **Per-edge rotation**: after histogram peak identifies inliers, fit a
   weighted line (perp = slope × along + intercept) to the inlier
   (along, perp) pairs. The slope gives per-edge rotation (capped at ±3°).
   This handles cases where BDTOPO got the edge angle slightly wrong.

3. **Elongation**: extend edge endpoints to cover the inlier along-range,
   capped at ±30% of original edge length.

4. **snapped-edges-preview artifact**: pointcloud showing each edge's final
   position (shifted + rotated + elongated) before polygon reconstruction.
   Dense sampling at 10cm intervals at eave height.

5. **Improved reconstruction**: `reconstructPolygon()` now uses newP1/newP2
   (incorporating offset + rotation + elongation) to define each edge line,
   then intersects consecutive lines for corner vertices. Fallback: midpoint
   of adjacent edge endpoints.

6. **Self-intersection fix on revert**: when reverting an edge involved in
   self-intersection, also reset its newP1/newP2 to original footprint
   position (not just offset=0).

### Result on dahouet

```
23/24 edges snapped (22 after 1 revert for self-intersection)
Per-edge rotations: -3.00° to +3.00° (many hitting the cap)
WECS: 0.887 → 0.932 (good)
Area ratio: 1.08 (190.4 → 206.1 m²)
```

Improved from v4's WECS 0.917 thanks to more candidates (all bits) and
per-edge rotation.

## The fundamental limitation

Even with rotation and elongation, snap-footprint v5 has a structural
weakness: **it uses the BDTOPO footprint to segment the candidate cloud**.

Each BDTOPO edge acts as a query: "which candidates are near me?". The
edge's position and angle determine which candidates get assigned to it.
If the BDTOPO edge is 2m off, candidates near the true building edge may
be closer to the *wrong* BDTOPO edge, causing misassignment.

This became clear when analyzing the existing edge detection steps:

### How wall-roof-detect does it (dirBin connected components)

Phase 1 of wall-roof-detect detects footprint edges purely from LiDAR:
1. Group boundary candidates by `dirBin` (8 bins × 45° from detect-boundary)
2. Within each direction group, run connected components in XZ (k-NN graph)
3. PCA line fit per component → footprint edge segment

**Problem for 24-edge buildings**: only 8 direction bins means ~3 edges per
bin. Edges in the same bin that are spatially close merge into one connected
component. The 45° quantization was designed for simple rectangular buildings
(4 walls → 4 bins, clean separation).

### How ridge-edge-fit does it (segment-pair clustering)

Ridge-edge-fit splits candidates by which two plane segments they border:
1. For each candidate, find own segment + dominant neighbor segment via k-NN
2. Group by sorted segment pair key (e.g. "2-5")
3. Cluster + PCA fit within each pair group

**Not applicable for footprint edges**: this needs the `owner` array from
plane segmentation, which runs after footprint snapping in the pipeline.

### PCA line fit (shared by both)

Both approaches converge to the same fitting step:
1. Covariance matrix on the candidate cluster
2. Power iteration → largest eigenvector = line direction
3. Elongation = λ1/λ2 (reject if < 2.5-3.0, not line-shaped)
4. Project points onto direction → line extent (from/to endpoints)

## Segmentation alternatives considered

The core problem: ~2000 boundary candidates form a ring around the building.
We need to segment them into ~24 groups before PCA can fit lines.

### No prior (pure LiDAR, no footprint needed)

**A1. Hough transform** — each candidate votes in (θ, ρ) space. Peaks =
lines. Finds all lines simultaneously. Handles any number of edges.
Struggles with short edges (few votes).

**A2. Sequential RANSAC** — pick 2 random candidates, fit line, count
inliers, extract best, repeat. Simple but greedy (order-dependent, can
steal points from nearby parallel lines).

**A3. Direction-continuity region growing** — compute local direction per
candidate (from small k-NN PCA or dirBin), grow regions of consistent
direction. Adaptive, no fixed bin count, but sensitive to direction noise.

**A4. Clustering in (x, z, θ) space** — DBSCAN in combined
position+direction space. Naturally separates parallel walls (different XZ,
same θ). Hard to weight position vs angle correctly.

### Weak prior (dirBin from detect-boundary)

**B1. DirBin connected components** — current wall-roof-detect. Too coarse
for 24 edges (8 bins × 45°).

**B2. Finer direction quantization** — use raw angle instead of 8-bin
quantization. Would require detect-boundary to store continuous angle.

**B3. Direction-weighted graph cut** — k-NN graph on candidates, cut edges
with low direction similarity. No fixed bin count but needs threshold tuning.

### Strong prior (BDTOPO footprint)

**C1. Nearest-edge assignment** — current snap-footprint v5. Projects
candidates onto nearest footprint edge. Handles 24 edges trivially but
fails when BDTOPO is significantly misplaced.

**C2. Voronoi assignment on edge normals** — assign by perpendicular
distance instead of Euclidean. Better for offset edges but same failure
mode as C1 for large misplacements.

**C3. Footprint-guided EM** — initialize from BDTOPO (C1), PCA fit lines,
re-assign candidates to nearest fitted line, repeat. Self-correcting but
more complex.

**C4. Footprint as seed + constrained RANSAC** — RANSAC biased toward each
BDTOPO edge's direction. Fast, robust to outliers, but still per-edge.

## Chosen strategy: detect-then-match

### Rationale

We want a **no-prior solution** because:
1. The goal is to find edges, then associate footprint edges to found ones
   (not the reverse)
2. Buildings can share walls — neighboring buildings contribute edges to
   the same candidate cloud. A no-prior detector naturally handles this
3. The BDTOPO footprint should constrain the **matching**, not the
   **detection**

### Plan

**Phase 1 — Edge detection (no footprint knowledge)**

Run on all boundary candidates (bits 0,1,4,5,6,7) in XZ plan view:

1. **Hough transform**: vote in (θ, ρ) parameter space. Extract peaks
   above a support threshold. Each peak = one detected edge with a score
   based on support count and weight sum.

2. **RANSAC on residuals**: candidates not claimed by Hough peaks get
   sequential RANSAC line extraction. Catches short edges that Hough
   missed.

3. **Merge**: `mergeCollinearEdges()` to deduplicate near-parallel
   overlapping edges from the two detectors.

Each detected edge has: `{ from, to, direction, score, supportCount,
weightSum, elongation, source: 'hough'|'ransac' }`.

**Phase 2 — Footprint matching (topology-aware assignment)**

For each building footprint, find the best assignment of detected edges
to footprint edges:

1. Build a cost matrix: `cost(detected_i, footprint_j)` based on angular
   compatibility, perpendicular distance, and length overlap.

2. **Dynamic programming alignment** (not Hungarian): the footprint is an
   ordered sequence of edges forming a polygon. The matcher walks the
   footprint edge sequence and picks the best matching detected edge for
   each, with transition costs that enforce spatial consistency
   (consecutive matched edges should meet at corners). Unmatched footprint
   edges are allowed (skip penalty).

   This is analogous to sequence alignment (Viterbi/DTW) — the footprint
   is the reference sequence, detected edges are the candidate pool.

3. Output: per-footprint-edge match (detected edge or "no match"), plus
   a global match quality score.

**Phase 3 — Polygon reconstruction**

Replace matched footprint edges with their detected edge lines. Intersect
consecutive edge lines for corner vertices. Unmatched edges keep their
BDTOPO position. Validate (simple polygon, area ratio).

### Why this is better

| Property | v5 (project-based) | New (detect-then-match) |
|----------|-------------------|------------------------|
| Segmentation | BDTOPO-dependent | LiDAR-intrinsic |
| Shared walls | Can't handle | Natural (one detected edge, multiple matches) |
| Edge angle | Regression on projected inliers | PCA on spatial cluster |
| Missing evidence | Silent (keeps BDTOPO position) | Explicit (no match → known gap) |
| Neighboring buildings | Candidates mixed in | Detected as separate edges, filtered by matching |
| Confidence | Implicit (coverage + peak count) | Explicit score per detected edge |

### Risks and unknowns

- **Hough bin resolution**: too fine → fragmented peaks, too coarse → merged
  lines. Needs tuning (θ bin ~0.5-1°, ρ bin ~0.1-0.15m probably).
- **Short edges**: a 1m edge with ~10 candidates may not produce a strong
  Hough peak. RANSAC fallback should help.
- **Dense parallel edges**: L-shaped buildings have parallel walls ~5m apart.
  Hough produces two peaks at same θ, different ρ — this should work.
- **DP matcher complexity**: with ~50 detected edges and ~24 footprint edges,
  the state space is manageable (50^24 is huge but DP with pruning reduces
  to O(N×M) per edge).
- **Elongation filter**: short edges might fail the λ1/λ2 test. May need
  a lower threshold for short candidate clusters.

## Next steps

1. Implement Hough transform on boundary candidates in XZ
2. Test on dahouet (24 edges) to see what lines are detected
3. Implement RANSAC on residuals
4. Implement DP topology matcher
5. Compare WECS with v5 approach
6. Benchmark on broader test set

## Files involved

| File | Role |
|------|------|
| `lib/pipeline/steps/snap-footprint.js` | Current v5 implementation (to be replaced) |
| `lib/pipeline/steps/detect-boundary.js` | Produces boundary candidates (bits 0-7) |
| `lib/pipeline/steps/detect-boundary.md` | Documentation of bit table and pseudo code |
| `lib/pipeline/steps/wall-roof-detect.js` | Inspiration: dirBin + connected components + PCA |
| `lib/pipeline/steps/ridge-edge-fit.js` | Inspiration: segment-pair clustering + PCA |
| `lib/geometry/edge-fitting.js` | `fitLine3D`, `lineExtent`, `clusterAndFitEdges`, `mergeCollinearEdges` |
| `lib/geometry/pca.js` | `covarianceMatrix`, `powerIteration`, `eigenvalue`, `deflate` |
