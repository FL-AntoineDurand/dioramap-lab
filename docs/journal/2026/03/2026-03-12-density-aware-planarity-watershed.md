---
id: "2026-03-12-density-aware-planarity-watershed"
date: 2026-03-12
title: "Density-Aware Planarity Watershed (V2)"
tags: [planarity-watershed, density-compensation, weighted-fit, axis-10]
category: development
status: concluded
refs: ["2026-03-12-local-density-compensation", "2026-03-07-ten-axis-research-session-report"]
pipeline: exploration
---

## Context

The planarity-watershed step (Axis 10) performs watershed segmentation on the planarity scalar field from tile-signals. Each point flows toward its neighbor with highest planarity; convergence to the same local maximum defines a basin (roof segment). The step was implemented as v1 using raw point counts for size thresholds and unweighted plane fits.

The density compensation analysis (`2026-03-12-local-density-compensation`) identified that LiDAR non-uniform density (beam geometry 2:1, strip overlap 2x, occlusion shadows) biases all counting-based operations. Basin size checks, plane fits, and boundary detection are all affected.

## Changes (v1 -> v2)

### Weight contract

Consumes the optional `point-weights` artifact (m² per point, Voronoi cell area):
- `w_i = pi * dk^2 / k` — raw m², not normalized
- Range 0.01-1.0 m² typical for LiDAR HD (2-50 pts/m²)
- `sum(weights)` approximates total surveyed surface area
- Fallback: `new Float32Array(n).fill(1.0)` — each point "represents 1 m²", so sum = n and old thresholds still work

### Phase-by-phase changes

| Phase | V1 (point-count) | V2 (density-aware) |
|-------|-------------------|---------------------|
| 1. Flow | kNN highest planarity | No change (topology only) |
| 2. Basin assignment | Follow flow to attractor | No change (topology only) |
| 3. Small-basin merge | `length >= minBasinSize` (10 pts) | `sumWeights(basin) >= minBasinArea` (0.5 m²) |
| 3. Residual | `sqrt(sum(d²) / n)` | `sqrt(sum(w*d²) / sum(w))` |
| 3-5. Plane fits | `fitPlane(pts)` | `fitPlaneWeighted(pts, weights)` |
| 6. Boundary | kNN — any neighbor in different basin | `radiusSearch(0.5m)` + `minBoundaryNeighbors >= 3` guard |

### New geometry: `fitPlaneWeighted`

Added to `lib/geometry/pca.js`. Weighted centroid (`sum(w*p) / sum(w)`) and weighted covariance (`sum(w*(p-c)(p-c)^T) / sum(w)`), same Cardano eigensolver. With uniform weights, produces identical results to `fitPlane` (verified).

### New params

| Param | Default | Unit | Description |
|-------|---------|------|-------------|
| `minBasinArea` | 0.5 | m² | Replaces `minBasinSize` (10 pts) |
| `boundaryRadius` | 0.5 | m | Radius for spatial boundary search |
| `minBoundaryNeighbors` | 3 | count | Sparse-area guard for boundary detection |

### Boundary detection improvement

V1 used kNN, which in sparse zones would reach far neighbors and produce noisy boundaries. V2 uses `radiusSearch` (fixed 0.5m radius) with a minimum-neighbor guard: if fewer than 3 points are within the radius, the point is not marked as boundary (insufficient local evidence). This pattern matches cascade-ridge-detect v4.

## Wiring

`point-weights` routed from `tile-signals` to `planarity-watershed` in the exploration pipeline. The artifact is optional — the step checks `ctx.hasArtifact('point-weights')` and falls back to uniform weights if not present.

## Verification

- `fitPlaneWeighted` with uniform weights matches `fitPlane` exactly (dot product = 1.0, RMS delta < 0.001)
- All 43 PCA tests pass
- Full test suite: 2006/2011 pass (5 pre-existing failures unrelated to watershed)
- Step loads correctly with updated meta (version 2, 8 params, 5 reads)
