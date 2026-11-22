---
id: "2026-03-15-planes-from-owner-wall-fixes"
date: 2026-03-15
title: "planes-from-owner v4: classification filter removal + coplanar merge"
tags: [planes-from-owner, walls, segmentation, coplanar-merge, v7]
category: bugfix
status: concluded
refs: []
pipeline: v7
---

## Problem

Investigation on `real-22186-dahouet-502` (Dahouet building) revealed two issues in `planes-from-owner` that caused wall planes to be lost or fragmented.

### Issue 1: LAS classification filter kills real walls

Group 40 (96 pts, clearly a wall at tilt=90°) was rejected by the classification majority-vote filter. Breakdown: c5(high veg):39, c1(unclassified):36, c2(ground):9, c4(med veg):7, c6(building):4, c3(low veg):1. Only 40/96 (42%) were keep-classes (0,1,6), below the 50% threshold.

The LiDAR classifier had tagged wall points as vegetation — a common issue where trees/hedges against walls cause misclassification of the wall surface itself. The classification filter was designed to remove vegetation groups, but on walls with adjacent vegetation it removes real building geometry.

### Issue 2: Coplanar wall fragments from upstream normal noise

Groups 32 (144 pts) and 41 (52 pts) were two separate owner groups for the exact same wall:
- Normal dot product: 1.0000 (identical orientation)
- Plane offset: 0.004 m (coplanar)
- Centroid distance: 0.94 m (overlapping)
- BBox XZ overlap: true

**Root cause** traced through the full segmentation chain:

1. **Per-point normals are inconsistent on walls.** Group 32 mean raw normal: (-0.477, 0.300, 0.485). Group 41 mean raw normal: (0.250, 0.020, -0.247). Individual points alternate between (0.67, 0, -0.74) and (-0.67, 0, 0.74) — opposite directions on the same wall surface. This is inherent to overhead LiDAR scanning geometry on vertical surfaces.

2. **Lambert hemisphere projection** (Stage 1 in `hemisphere-normals-density-aware-segment`) uses `Math.abs(ny)` to fold normals to the upper hemisphere, so both orientations map to the same bin. No split here.

3. **Offset KDE** (Stage 2) computes `offset = dot(meanNormal, position)`. The `meanNormal` is the weighted average of **raw** (unfolded) normals within the Lambert group. When raw normals are mixed-sign, the mean normal is noisy, and the offset projection scatters points into different KDE peaks. Both groups share segments from normalGroupId 1 (offsetClusterId 2) and normalGroupId 3 (offsetClusterId 0) — confirming the split originates in Stage 2.

4. **Spatial connectivity** (Stage 3) and **competitive-grow/point-stealing** cannot re-merge them because they're already competing as separate owner IDs.

## Fix

### Classification filter removed (v3)

The LAS classification filter was removed entirely. Building extraction already happens upstream in `extract-denoise` using LAS classes. By the time points reach `planes-from-owner`, they've already passed through segmentation — the classification filter was a redundant second gate that rejected legitimate wall geometry.

On Dahouet: 24 planes → 61 planes (37 previously class-rejected groups now emitted).

### Coplanar merge pass added (v4)

After refitting all groups, a pairwise coplanarity check merges fragmented groups:

1. **Normal angle** < 10° (using `abs(dot)` to handle flipped normals)
2. **Plane offset** < 0.3 m
3. **Hull gap** < 2.0 m (nearest hull vertex distance — prevents merging parallel walls on different facades)

Union-find clusters coplanar+adjacent groups, pools their points, refits a single plane equation, and recomputes the hull.

On Dahouet: groups 32+41 merged into a single 196-point wall plane (tilt=89°, area=33.0 m²). Group 40 (parallel wall 3.8 m away) correctly stayed separate.

Result: 61 groups → 60 emitted planes (1 merge).

## Key insight

The upstream normal noise problem on walls is fundamental to overhead LiDAR — vertical surfaces get inconsistent normal estimates because the scan angle is nearly parallel to the surface. Rather than trying to fix normal orientation upstream (which would require knowing the building interior, fragile for concave/L-shaped buildings), the coplanar merge in `planes-from-owner` catches the symptom robustly regardless of cause.
