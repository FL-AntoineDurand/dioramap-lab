---
id: "2026-03-19-boundary-edges-development"
date: 2026-03-19
title: "boundary-edges: semantic grid + boundary walk + ridge geometry"
tags: [boundary-edges, adjacency, ridge, walk, grid, edge-detection, v7]
category: development
status: concluded
refs: ["2026-03-16-adjacency-boundary-edge-detection", "2026-03-15-planes-from-owner-wall-fixes"]
pipeline: v7
---

## Context

After segmentation (cascade-seed → grow → steal), plane regions have no explicit topology — no edges, no adjacency, no boundary shape. The goal: detect all plane boundary edges (ridges, eaves, dormers) and build closed polygons per plane.

## Three parallel approaches developed

### A. Semantic Grid (surface domain, 5cm resolution)

Rasterizes all evidence onto a 2D grid on each plane's tangent space.

**Pipeline (weakest → strongest, later overwrites earlier):**
1. LINEARITY cells (±15cm, aligned with boundary direction)
2. MAX-CURVE cells (±15cm, validated: peak curvature perpendicular to boundary)
3. GSR-RIDGE cells (±15cm, ridge candidates from grow-steal-resolve)
4. EAVE cells (±20cm, height-gap boundary from detect-boundary)
5. MULTI-RETURN cells (±25cm, physical beam-split edge markers)
6. RIDGE cells (single line, exact plane-plane intersection)
7. PLANE cells from group points (weighted splat, spacing×0.6 floor)

**Key design: two-buffer system.** `baseGrid` (snapshot after PLANE fill) used for boundary direction detection. `grid` (with all labels) used for visualization. Prevents signal labels from corrupting direction gradient.

**Direction detection** for all signal types: fixed-radius search of own-group points → weighted center of mass → perpendicular to inward direction. Type-specific validation (MAX-CURVE checks peak along perpendicular, GSR-RIDGE checks balanced split, etc.)

### B. Boundary Walk (point domain)

Robot walker traces the plane perimeter by following the weighted center of mass direction.

**Algorithm:**
1. Start at nearest group point to longest ridge endpoint
2. At each step: compute weighted CoM of own-group points in adaptive radius (spacing×3 to ×12, min 14 points). Inward = toward CoM, boundary = perpendicular. Apply chirality.
3. Project forward by step size (~45cm). Find outermost group point near projection along outward direction.
4. Ridge rail: when near a ridge line and aligned (cos > 0.5), mount and ride along. Check local direction every step — leave if diverges (> 60°).
5. Corner scan: if stuck (moved < 15% of step), try rotated directions ±20° to ±120°.
6. Loop detection: coarse grid of visited cells, break after 3 consecutive revisits.
7. Terminate when close to start (closed polygon) or max steps.

**Confidence score**: (count/30) × (2/usedRadiusMultiplier). High count + small radius = high confidence. Rendered as dark→bright green gradient in SVG.

### C. Ridge Geometry (exact from plane equations)

- Adjacency graph from point ownership (fixed 0.5m 3D radius, min 3 contacts)
- Ridge line = plane-plane intersection line, clipped to XZ-projected group extents
- Type classification: ridge/hip/valley/flashing/step/corner from plane normals + centroid heights
- Corners: line-line closest approach (within intersection extent + 1m)
- **Filters**: contact density per meter consistent with local density AND union/intersection overlap ratio > 15% (removes corner-only false ridges)

## Key findings

### point-weights = area in m² directly
tile-signals emits `π×d_k²/k` which IS the Voronoi area. Many steps were double-dividing by density (`pointWeights[gi] * invDensity`). Fixed everywhere.

### No NOGO zones
Above/below point projection for forbidden zones is unreliable: eave overhangs create LiDAR points under the roof that project as false NOGO on the plane. Removed entirely.

### XZ projection for ridge extents
3D projection of group points onto tilted ridges (hips) causes overshooting — height differences amplify the extent. XZ-only projection fixes this for all non-vertical ridges.

### Wiring matters
`point-weights`, `curvatures-k32`, `linearity`, `lidar-density` must be explicitly wired from `tile-signals#0` in the v7 pipeline — auto-resolver can't find them through dead-end steps.

### Classification filter was harmful
LAS classification majority-vote in planes-from-owner killed real walls (vegetation misclassification). Removed. Building extraction is upstream in extract-denoise — redundant filter.

### Coplanar merge works
Union-find on normal angle (< 10°) + plane offset (< 0.3m) + hull gap (< 2m) successfully merges wall fragments from upstream normal noise.

### Walk limitations
- Sharp convex corners (> ~60°) stall the walker — CoM-based direction doesn't rotate fast enough
- Corner rotation scan (±20° to ±120°) partially fixes but is greedy (takes first valid, not best)
- Most planes don't close — the walker stops at loop detection or max steps
- Walk is useful for topology visualization but not yet for polygon extraction

## Visualization

3-tab SVG debug overlay in the pipeline graph view:
- **Points**: signal points (shape by type, color by weight), group points (teal), polygon edges (type-colored, solid/dashed)
- **Grid**: semantic cells (9 types), base/label layer toggle
- **Walk**: group points with weight circles, robot walk path with CoM confidence gradient, ridge rail segments, direction arrows

3D view: two-color highlight (teal=group, orange=nearby), type-colored edges, corner markers.

## Status

Three approaches working in parallel but not yet unified. Ridge geometry gives exact edges. Walk gives topology direction. Grid gives surface-level boundary evidence. Next: combine into final polygon edges per plane.
