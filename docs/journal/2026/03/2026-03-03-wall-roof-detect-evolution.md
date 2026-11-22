---
id: "2026-03-03-wall-roof-detect-evolution"
date: 2026-03-03
title: "Wall-Roof Detection: From 3D K-NN Failure to 2D Dual-Edge Fitting"
tags: [edges, wall-roof, debugging, sensor-params]
category: development
status: concluded
refs: ["2026-03-03-multi-source-edge-detection"]
---

## Problem

Detect building footprint boundary edges from the raw point cloud — where roof
meets ground (or a lower roof level). These edges are invisible to `ridge-clip`
which only sees plane-pair intersections.

## Evolution (v1 → v8)

### v1–v4: 3D K-NN — Complete Failure

Used standard 3D k-NN (same as other steps) with radius ~0.5m.
**Result**: 0 real detections. Ground points are 6+ meters below roof
in Y but directly adjacent in XZ. A 0.5m 3D radius never reaches them.

**Lesson**: Height discontinuity detection fundamentally requires 2D
(plan-view) spatial search, not 3D.

### v5: 2D K-NN — Too Many False Positives

Switched to XZ-only k-d tree (ignoring Y in distance metric).
Used `k2d = 2 × k` (48 neighbors) to compensate.
**Result**: 7695 candidates (50% of building points). Every noisy point
triggers because wide k pulls in distant points with different heights.

### v6: Bimodal Balance Check — Clean Candidates

Tightened `k2d = k` (24 neighbors). Added bimodal balance requirement:
both sides of the height gap must have ≥3 points.
**Result**: 1333 candidates (8.7%). Clean perimeter ring around building.
User confirmed: "wall-roof-candidates is very good now."

### v7: Direction-Aware Grouping — Multiple Edges

All 1333 candidates formed one connected ring (BFS connects the
entire perimeter). Solution: quantize gap direction (roof→ground
centroid vector in XZ) into 8 bins × 45°, cluster within each bin,
merge collinear edges from adjacent bins.
**Result**: 40 raw edges → 9 after merge. But placed at mean height
(midpoint between roof and ground) and all horizontal.

### v8: Dual Edges with 3D Tilt — Final Version

Split each 2D cluster by `gapMidY` into upper (roof-level) and lower
(ground-level) point groups. Fit 3D PCA line through each sub-group
independently.

**Result**: 21 edges with proper characteristics:
- Ground edges at Y≈0, roof edges at Y≈6–14
- Flat walls at 0° tilt, gable transitions at ~19°
- Tower diagonal edges at 67–72° tilt (following steep gable slope)

## Key Algorithm (v8)

1. Build 2D (XZ) k-d tree from ALL scene points (building + ground)
2. For each building point, query k-NN in XZ plane
3. Analyze Y distribution of neighbors: find largest gap in sorted Y values
4. Check gap dominance (`maxGap × (count-1) / spread ≥ threshold`)
5. Check bimodal balance (≥3 points on each side of gap)
6. Record gap direction (XZ angle from roof centroid to ground centroid)
7. Quantize direction to 8 bins × 45°, cluster within each bin (2D XZ)
8. Per cluster: split by gapMidY → upper group + lower group
9. Fit 3D PCA line through each group → two edges per wall segment

## Parameters (auto-derived from sensor)

| Param | Auto value | Source |
|-------|-----------|--------|
| `gapThreshold` | `max(0.5, 10 × σ_z)` | Rejects noise-level gaps |
| `minClusterPts` | `deriveMinRegionPts(ρ, 0.3)` | Scales with density |
| `minElongation` | 3.0 | Rejects round blobs |
| `gapDominance` | 5.0 | Requires clear bimodal split |
