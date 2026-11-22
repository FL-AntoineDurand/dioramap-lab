---
id: "2026-03-16-adjacency-boundary-edge-detection"
date: 2026-03-16
title: "Adjacency graph + boundary polygon edge detection"
tags: [adjacency-edges, boundary-edges, planes-from-owner, grid-hull, v7, edge-detection]
category: development
status: concluded
refs: ["2026-03-15-planes-from-owner-wall-fixes"]
pipeline: v7
---

## Context

After the v7 segmentation chain (cascade-seed → grow → steal → planes-from-owner), we have plane regions with hulls but no explicit topology. The existing `ridge-clip` step discovers adjacency indirectly via hull proximity — expensive and misses cases. We need a direct, signal-driven approach to detect all plane boundary edges: ridges, valleys, eaves, and dormers.

## Grid-rasterize hull (planes-from-owner v5)

The alpha-shape hull from `concaveHull3DOnPlane` had two problems:
1. **Points excluded**: the alpha filter (circumradius threshold) drops isolated points, so some group points fall outside their own hull.
2. **Slow**: Delaunay triangulation + alpha filtering + RDP simplification took ~2.8s for 61 groups.

Replaced with a grid-rasterize + Moore boundary trace approach:
- Project group points onto the fitted plane → 2D
- Rasterize to grid (cell size = 1/√density ≈ 0.18m at 30 pts/m²)
- Dilate by 1 cell to close single-point gaps
- Moore boundary trace on the dilated grid → ordered polygon
- Back-project to 3D, snap to plane

All points are inside by construction (every occupied cell is interior). 42ms vs 2858ms — 68× faster. No alpha parameter to tune.

## Adjacency edges step

New step that builds the group adjacency graph directly from point ownership, then computes geometric edges and corners.

### Phase 1: Adjacency from point neighborhoods

For each building point, query kNN (K=8). If a neighbor belongs to a different owner group, record the pair as adjacent. Filter pairs with < 3 cross-boundary contacts.

Result on Dahouet: 50 adjacent pairs from 61 groups.

### Phase 2: Ridge edges

For each adjacent pair, refit plane equations from current points, compute plane-plane intersection line via `planeIntersectionLine()`. Project both groups' points onto the line to find per-group extent. Emit two edge types:
- **White (union)**: min/max of both groups' projected ranges — full extent of the ridge
- **Orange (intersection)**: overlap of both groups' ranges — where they actually meet

The white edges are further filtered by point support: walk along the edge in spacing-sized steps, radius-search for nearby points with `curvatures-k32 > 0.7` or `linearity > 0.45`. Only contiguous supported segments are emitted (`adjacency-supported` source).

### Phase 3: Corner markers

For each pair of ridge edges sharing a plane, compute the closest approach of the two 3D lines. If distance < 0.5m and within edge extents, emit as a corner marker. Deduplicate within 0.3m.

Result: 50 edges, 33 corners. New `markers` artifact type added for sphere rendering in UI.

## Boundary edges step

Fits a closed polygon perimeter for each plane using multiple signal sources from detect-boundary and tile-signals.

### Signal sources
- `boundary-candidates` from detect-boundary: eave (bit 1), dormer (bit 2), multi-return (bits 4,6), ground-proximity (bit 5)
- `ridge-candidates` from grow-steal-resolve: inter-plane boundary points
- `curvatures-k32` from tile-signals: high-curvature points
- `linearity` from tile-signals: high-linearity points

### Signal weighting
Each source type has a base weight reflecting its precision:
- Multi-return (5.0) — precise physical edge markers (beam split)
- Eave (3.0) — height gap with ground below
- Dormer (2.5) — height gap with building below
- Ridge (2.0) — boosted for outer points of the band (distance from centroid)
- Curvature/linearity (1.0) — noisy but abundant

### Polygon fitting
Per plane: project all group points + kNN neighbors onto the plane → 2D. For each projected point, compute signal strength. Sort signal points by angle around centroid. Fit a closed N-gon (N=3,4,...12) by dividing the angular range into N sectors, picking the outermost high-weight point per sector. Refine vertex positions to minimize weighted RMS. Stop adding vertices when RMS improvement drops below 10%.

Result: most planes get 3-5 edges (triangles, quads, pentagons). Connected edges forming closed polygons.

## Known limitations / next steps

1. **Signal strength is per-plane relative** — current implementation computes weight globally. A ridge between planes A and B should have high weight for both A and B, but zero for plane C. Needs per-plane contextualization using owner information.
2. **Ridge band treatment** — ridge candidates form a wide strip. Current outer-point weighting helps but isn't precise. Need to weight based on which side of the ridge the point is on relative to the current plane.
3. **Per-plane 2D visualization** — need a clickable per-plane 2D scatter plot showing projected signal points colored by strength with polygon overlay, for debugging the fitting.
4. **Polygon vertex optimization** — current sector-based placement is a heuristic. Could benefit from gradient-based optimization or simulated annealing for vertex positions.
