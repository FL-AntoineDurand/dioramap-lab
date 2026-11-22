---
id: "2026-03-06-axis2-multiview-rasterize-detect"
date: 2026-03-06
title: "Axis 2: Multi-View Rasterize-Detect Plane Detection"
tags: [research, plane-detection, multiview, raster, connected-components, normal-histogram, offset-histogram, coplanar-merge, speed-optimization]
category: experiment
status: concluded
refs: ["2026-03-06-four-axis-research-synthesis"]
pipeline: standalone-detector
---

## Summary

This entry documents the full research trajectory of Axis 2: a plane detection algorithm that projects LiDAR point clusters onto 2D raster "views" perpendicular to each detected normal direction, then uses connected component analysis to spatially segment those projections. The "multi-view" insight is that each projection is equivalent to looking at the building from the direction of a candidate plane's normal, revealing the spatial extent of that plane's points while collapsing depth along the normal axis.

The approach evolved from a 74.9-composite first implementation through 12 distinct versions and 5 alternative architectures (Sobel edge barriers, region growing, hybrid height-variance, DSM gradient-only, speed-optimized), culminating in a champion that achieves:

- **Quick (21 cases): 97.2 avg composite, 100% matchRate**
- **All-synth (150 valid cases out of 159): 95.1 avg composite, 93.9% matchRate, 129 with perfect matchRate**
- **v1.4 speed-optimized variant: identical quality at 5-15x less wall-clock time**

## Motivation and Hypothesis

The core hypothesis was that projecting a 3D point cloud onto multiple 2D raster views from different orientations would reveal roof edges as sharp density transitions. The top-down (plan) view shows eave/ridge outlines; elevation views show height breaks. By combining edge information from multiple views, we could infer plane boundaries and fit planes within bounded regions. This would directly detect edges (which existing pipelines infer indirectly from plane-plane intersections).

In practice, the "multi-view" concept narrowed to a specific realization: for each detected normal direction, project that cluster's points onto the plane perpendicular to the normal. Connected components in this 2D raster reveal contiguous spatial patches. Full multi-view edge detection (Hough transform, auto-focus, per-strip rasterization) was planned but blocked by the harness not passing LiDAR metadata (scanAngles, pointSourceIds, returnNumbers).

## Complete Architecture of the Champion (v1.0, detector.js)

Seven sequential phases:

```
Input: Float32Array positions, point count
  |
  v
[1] PCA normal estimation (k=15 neighbors, grid spatial index)
  |
  v
[2] Hemisphere normal histogram (20x20 bins, greedy peak merging at 8 deg)
  |
  v
[3] Per-peak offset histogram (bin=0.08m, valley detection)
  |
  v
[4] Per-offset-group 2D raster CC (adaptive cell = max(0.1m, 1.5 * avgSpacing))
  |
  v
[5] PCA refit + inlier filter (threshold 0.12m, double refit)
  |
  v
[6] Coplanar merge (angle < 12 deg, offset < 0.15m, point proximity < 2.5 * avgSpacing)
  |
  v
[7] Unmatched point assignment (closest plane within 0.25m)
  |
  v
Output: surfaces [{type:'plane', normal:[nx,ny,nz], d, pointIndices:[...]}]
```

### Phase 1: PCA Normal Estimation (k=15)

For each point, find k=15 nearest neighbors using a 3D grid spatial index (cell size = 2.5 * avgSpacing, where avgSpacing = cbrt(extent^3 / count)). Build the 3x3 covariance matrix C of the neighborhood's position deltas from their centroid. Extract the smallest eigenvector via the closed-form Cardano cubic solution: compute trace q, off-diagonal energy p, determinant of the shifted matrix, and solve for eigenvalue lambda_3 = q + 2p*cos(phi + 2pi/3). The null-space vector of (C - lambda_3*I) is the normal. Orient all normals to the upper hemisphere (ny >= 0).

The grid uses 27-cell neighborhood lookup (3x3x3 surrounding cells) and sorts candidates by squared distance. This phase dominates runtime in v1.0 -- fixed in v1.4 by replacing the sort with expanding-shell lookup.

### Phase 2: Hemisphere Normal Histogram (20x20 bins)

Map each normal to a 2D histogram bin using upper-hemisphere spherical coordinates:
- theta = atan2(nz, nx), mapped to [0, gridRes) via (theta + pi) / (2*pi) * gridRes
- phi = acos(ny), mapped to [0, gridRes) via phi / (pi/2) * gridRes

This parameterization covers the upper hemisphere uniformly. Each bin accumulates point indices. Bins are sorted by count (descending) and processed greedily: each bin attempts to merge into the first existing peak whose mean normal has dot product >= cos(8 deg). If no peak matches, a new peak is created. After all bins are assigned, each peak's normal is recomputed as the normalized mean of all constituent point normals. This averaging step refines the peak normal beyond the coarse bin center.

**Why 8 degrees**: Row houses have adjacent units with 7-degree normal differences. At angleThresholdDeg=8, the algorithm just barely separates them. At 10+, units merge. At 5, over-segmentation appears on other cases.

### Phase 3: Offset Histogram (bin = 0.08m)

For each normal peak, compute the signed distance (offset) of each point from the origin along the peak's normal: d = dot(n, p). Build a 1D histogram with bin width 0.08m. Split at "valleys" -- bins with count below max(1, 0.15 * avgBinCount). Contiguous runs of non-valley bins form offset groups. This separates parallel planes at different heights (e.g., a flat roof and an AC unit 0.5m above it, or multi-level flat roofs).

**Why 0.08m**: AC units (small elevated structures) sit ~0.1-0.3m above the main roof. At binSize=0.15m+, the AC unit's offset peak merges with the main roof peak. At 0.05m, slight over-segmentation on noisy data.

### Phase 4: 2D Raster Connected Components (adaptive cell)

For each offset group, project points onto the plane perpendicular to the group's normal. Construct orthonormal tangent basis:
- If |ny| < 0.9: u = normalize(nz, 0, -nx), else u = normalize(0, -nz, ny)
- v = n x u (cross product)

Project each point: (u_coord, v_coord) = (dot(p, u), dot(p, v)). Build a 2D raster grid with adaptive cell size = max(0.1m, 1.5 * globalAvgSpacing). Mark cells as occupied/empty and run 4-connected flood fill to extract connected components. Components with < 8 points are discarded.

The adaptive cell size prevents both too-coarse cells (missing genuine gaps between spatially separated planes) and too-fine cells (fragmenting continuous planes through noise-induced raster holes). A safety cap of 2M cells prevents memory explosion on degenerate cases.

### Phase 5: PCA Double Refit

For each candidate plane, fit via PCA (smallest eigenvector of covariance). Filter outliers: points farther than 0.12m from the fitted plane are discarded. Refit on the inlier set. This two-pass refinement significantly improves normal accuracy on noisy data where the initial clustering assigns some off-plane points.

### Phase 6: Coplanar Merge

Iteratively merge plane pairs satisfying ALL three criteria:
1. **Normal angle** < 12 deg: |dot(n_i, n_j)| >= cos(12 deg)
2. **Offset difference** < 0.15m: |d_i - d_j| <= 0.15
3. **Point proximity**: sample up to 20 evenly-spaced points from plane A, check if ANY has a neighbor in plane B within 2.5 * globalAvgSpacing

The proximity check is critical. Without it (v0.9 bbox-only check), row house units at the same height and normal get merged back together, destroying the spatial split from Phase 4. With strict point-level proximity, only genuinely adjacent fragments merge. After each merge, the combined plane is refit via PCA.

### Phase 7: Unmatched Point Assignment

Points not claimed by any plane are assigned to the nearest plane (by point-to-plane distance along the plane normal) within 0.25m. This recovers edge points lost during inlier filtering or that fell between raster cells.

## Version History -- Complete Timeline

### v0.3 -- First Implementation (74.9 composite, 83.3% matchRate)

Initial pipeline: greedy normal clustering (assign each point to the most-similar existing cluster centroid) + offset histogram + 2D raster CC + PCA refit. Sanity results: 74.9 avg composite, 83.3% matchRate. `perfect-hip` scored 0.5 matchRate, `three-parallel-flats` scored 0.67.

**Root cause of failure**: Greedy normal clustering was order-dependent and did not refine centroids after assignment. The first point to create a cluster biased its centroid, causing later points with slightly different normals to scatter across clusters. No centroid recomputation meant the initial bias persisted.

### v0.4 -- Hemisphere Histogram Rewrite (93.6 sanity, 95.9 quick)

Replaced greedy clustering with hemisphere normal histogram (20x20 bins, sorted by count, greedy merge). Bin-based accumulation is order-independent. All 5 sanity cases passed (100% matchRate, 93.6 avg composite). Quick benchmark: 20/21 pass (matchRate >= 0.5), 95.9 avg composite, 96.8% matchRate. Only weak case: `proc-row_houses` at 0.33 matchRate. **This was the M1 and M2 gate pass.**

The +19 composite jump (74.9 to 93.6) came entirely from fixing the normal clustering: histogram binning is deterministic and captures the true mode structure of the normal distribution.

### v0.5 -- Spatial CC Before Offset (ABANDONED)

Hypothesis: separating disconnected patches FIRST, then splitting by offset, would help. Restructured pipeline to run raster CC before offset histogram. Result: **regression** to 81.7 avg composite on sanity, segmentation accuracy dropping across the board. Quick: 93.8 (comparable but worse segAcc on several cases).

**Why it failed**: Spatial CC applied to the full normal cluster is too aggressive. Noise creates small raster gaps in continuous planes, fragmenting them into spurious components. The offset histogram, being a 1D operation on a scalar, is cheap and robust. Running it first reduces per-CC point counts, making the finer raster affordable without fragmentation risk.

**Key architectural insight: offset split first, spatial CC second is the correct ordering.** This ordering exploits the fact that offset splitting is a cheap dimensionality-reducing pre-filter. It reduces the point count per CC operation by splitting parallel planes first, so each CC run operates on a smaller, more homogeneous set.

### v0.6 -- Residual-Triggered Spatial CC (ABANDONED)

Only trigger spatial CC when the plane's PCA residual exceeds a threshold. Theory: high residual indicates merged planes that need spatial splitting. Sanity improved to 99.3, but on quick: `flat_with_ac_unit` dropped to 0.5 matchRate, `cross_plan` to 0.46 segAcc.

**Why it failed**: Residual is an unreliable proxy for "needs spatial splitting." An AC unit adds residual to its host plane (the unit's points are off-plane), but splitting the host plane is wrong -- only the AC unit should separate. Meanwhile, genuinely merged coplanar planes may have low residual if they happen to be well-fit by a single plane equation. The correlation between "high residual" and "merged planes" is weak.

### v0.7 -- Adaptive Raster Cell Size (marginal)

Cell size formula: max(minCellSize, globalAvgSpacing * rasterCellSpacingMult). Adapts raster resolution to point density. With default multiplier, results were identical to v0.6 on quick (94.5). The adaptivity was necessary but not sufficient -- the parameters still needed tuning.

### M3 Parameter Exploration (v0.7-tuned / v0.8)

Systematic parameter sweep on the 21 quick cases identified three interacting problems:

1. **Row houses**: Three units with nearly identical pitch. Units 0 and 2 share exact same normal (0, 0.921, +/-0.39), unit 1 is only 7 degrees different (0, 0.828, +/-0.56). Units 0 and 2 are at the same height, separated only spatially. With the default 15-deg angle threshold, all three merge into one cluster.

2. **AC unit**: 87 points sharing the main roof's normal but sitting 0.5m higher. The default 0.15m offset bin was too coarse to create a valley between the main roof and AC unit histogram peaks.

3. **Cross-plan**: Four pairs of coplanar wing planes meeting at a junction where noisy normals bridge 0.3-0.5m gaps.

Three fixes applied simultaneously:
- `angleThresholdDeg = 8` (from 15): separates the 7-deg normal difference in row houses
- `offsetBinSize = 0.08` (from 0.15): detects the AC unit's height offset
- `minRasterCellSize = 0.1, rasterCellSpacingMult = 1.5`: finer spatial CC resolves row house unit separation

### v0.8 -- Best Params Baked In (96.7 quick, 100% matchRate)

Baked optimal parameters into defaults. All 21 quick cases passed with 100% matchRate. The +0.8 composite gain (95.9 to 96.7) came primarily from correctly detecting the AC unit (offset tuning) and separating row house units (angle + raster tuning).

### v0.9 -- First Coplanar Merge Attempt (PARTIAL)

Added post-processing merge pass inspired by cross-pollination from the Axis 4 (curvature) research agent. Two planes merge if normals within 12 deg, offsets within 0.2m, and bounding boxes overlap or are adjacent. Result: **regressed** row_houses from 1.0 to 0.83 matchRate.

**Why bbox merge fails**: Row house units are coplanar (same normal, same offset) and their bounding boxes overlap along the street direction (each unit extends slightly beyond its own footprint in the point cloud). The bbox adjacency check cannot distinguish "adjacent fragments of one plane" from "separate coplanar planes that happen to be near each other."

### v0.9b/v1.0 -- Strict Point-Proximity Merge (97.2 quick, 100% matchRate) -- CHAMPION

Replaced bbox check with sample-based point-level proximity: pick 20 evenly-spaced points from plane A, find the nearest point in plane B for each. If the minimum nearest-neighbor distance exceeds 2.5 * avgSpacing, the planes are not adjacent and must not merge. This preserved row_houses at 100% matchRate while merging genuine over-segmented fragments (e.g., a large flat roof split by a noise gap).

The +0.5 composite gain (96.7 to 97.2) came from merging over-segmented fragments that were hurting segmentation accuracy.

**v1.0 full quick results:**

| Case | composite | matchRate | segAcc | residualRMS | time |
|------|-----------|-----------|--------|-------------|------|
| gable-default | 98.6 | 1.0 | 0.979 | 0.032 | 5.3s |
| hip-default | 97.6 | 1.0 | 0.946 | 0.030 | 5.8s |
| flat-default | 98.7 | 1.0 | 0.984 | 0.031 | 5.3s |
| flat-large | 98.6 | 1.0 | 0.981 | 0.033 | 39.9s |
| asymmetric_gable | 98.4 | 1.0 | 0.974 | 0.032 | 5.5s |
| mansard | 96.8 | 1.0 | 0.920 | 0.030 | 3.6s |
| mono_pitch | 98.7 | 1.0 | 0.984 | 0.031 | 5.4s |
| l_shaped | 97.6 | 1.0 | 0.944 | 0.031 | 6.8s |
| flat_multi_level_2 | 98.7 | 1.0 | 0.982 | 0.031 | 6.4s |
| flat_multi_level_3 | 98.6 | 1.0 | 0.981 | 0.033 | 9.8s |
| flat_l_shaped | 98.7 | 1.0 | 0.984 | 0.031 | 6.8s |
| multi_section | 98.2 | 1.0 | 0.964 | 0.032 | 7.7s |
| industrial_stepped_2 | 98.7 | 1.0 | 0.983 | 0.032 | 27.8s |
| flat_with_ac_unit | 98.7 | 1.0 | 0.983 | 0.032 | 5.3s |
| proc-cross_plan | 81.9 | 1.0 | 0.475 | 0.023 | 0.7s |
| proc-dormer_house | 97.1 | 1.0 | 0.929 | 0.028 | 2.2s |
| proc-gambrel | 96.5 | 1.0 | 0.906 | 0.025 | 2.4s |
| proc-saltbox | 96.8 | 1.0 | 0.918 | 0.028 | 1.6s |
| proc-butterfly | 98.5 | 1.0 | 0.978 | 0.032 | 2.2s |
| proc-row_houses | 96.8 | 1.0 | 0.915 | 0.030 | 1.7s |
| proc-silo | 96.3 | 1.0 | 0.903 | 0.022 | 0.3s |

**Avg composite: 97.2/100, Avg matchRate: 100%, Avg residualRMS: 0.030m**

## Failed M4 Variations

### v1.1 -- Sobel Edge-Barrier CC (detector-sobel.js)

Applied Sobel gradient detection to the height raster in the normal-projected 2D plane. Hypothesis: height discontinuities within a cluster would appear as strong Sobel edges, serving as barriers in the CC flood fill. Quick result: 97.2 avg composite, 100% matchRate -- identical to v1.0. Cross-plan remained at 0.475 segAcc.

**Why it failed**: When points are projected onto the plane perpendicular to their shared normal, the height component along that normal is removed by construction. Coplanar planes have identical projected heights, so the Sobel filter finds zero gradient. This approach could only help for planes at different heights -- which the offset histogram already handles. The Sobel variant is fundamentally redundant with Phase 3.

### v1.2 -- Region Growing (detector-regiongrow.js)

Replaced the entire normal histogram pipeline with a top-down XZ raster approach. Each raster cell gets a local PCA normal from its points plus neighbors. Regions grow by merging adjacent cells whose local normals and offsets are compatible (angle < 10 deg, offset diff < 0.10m). Quick result: 94.4 avg composite, 96.4% matchRate.

Cross-plan improved to **0.721 segAcc** (90.9 composite) vs 0.475 in v1.0 -- a meaningful +0.246 improvement. But regressions elsewhere:
- `proc-dormer_house` dropped to 0.25 matchRate (dormers have complex normal transitions that confuse local PCA)
- `mansard` fell to 92.6 (mansard's curved transition zone between steep and shallow planes is hard for cell-level normals)
- `proc-silo` dropped to 87.4 (cylindrical surface produces continuously varying normals)

Region growing respects geometric boundaries at wing intersections (where local normals change rapidly), which helps cross-plan. But cell-level PCA normals are noisy for small clusters, and the growing process can either over-grow or under-grow depending on threshold sensitivity at complex features.

### v1.3 -- Hybrid Height-Variance Split (detector-hybrid.js)

Post-CC addition: for large connected components, build a top-down XZ raster of Y (height) values, compute gradient magnitude, use high-gradient cells as barriers for a secondary CC split. Quick result: identical 97.2. Cross-plan still 0.475.

**Same fundamental limitation as Sobel**: within a single normal cluster, coplanar planes have uniform height in the top-down view. The height gradient is zero across the entire cluster. Only if the planes were at different heights would gradient barriers form -- but that case is already handled by the offset histogram.

### DSM Gradient-Only (detector-dsm.js)

Alternative approach using top-down DSM height gradient for direct clustering, without the normal histogram pipeline. Much faster (~1.3s per case vs 5-40s). Quick result: 80.3 avg composite, 95.2% matchRate. The 17-point composite gap versus v1.0 confirms that normal-based clustering is essential -- DSM gradients alone cannot distinguish coplanar planes with different normals (e.g., the two halves of a gable roof at equal heights but different pitches).

## v1.4 -- Speed-Optimized Champion (detector-fast.js)

The normal estimation in v1.0 dominates runtime because it sorts all candidates in each grid cell's 27-cell neighborhood by distance -- O(n * m * log(m)) where m is the number of candidates per cell. For large cases (31K points in flat-large), m can exceed 1000 per point.

v1.4 replaces the sort-all-candidates approach with an expanding-shell grid lookup:
1. Build a finer spatial grid targeting ~8 points per cell (finer cell size than v1.0's ~27 pts/cell)
2. For each point, start with the home cell and expand outward in shells (ring of cells at Manhattan distance 1, then 2, etc.)
3. Stop expanding once k neighbors are found
4. Never sort large arrays -- just collect the first k points from expanding shells

This changes the effective complexity from O(n * m * log(m)) to approximately O(n * k), since each point only processes ~k candidates before stopping.

| Case | Points | v1.0 time | v1.4 time | Speedup |
|------|--------|-----------|-----------|---------|
| flat-large | 31K | 39.9s | 3.5s | 11.4x |
| industrial_stepped_2 | 24K | 27.8s | 3.0s | 9.3x |
| gable-default | 9K | 5.3s | 0.8s | 6.6x |
| proc-cross_plan | 3.5K | 0.7s | 0.17s | 4.1x |

Quality is identical: 97.2 avg composite, 100% matchRate on all 21 quick cases. All-synth validation confirmed identical results (95.1 avg, 93.9% matchRate) in ~5 minutes vs ~35 minutes for v1.0.

## Score Progression Summary

| Version | Quick Composite | Quick matchRate | Key Change | Status |
|---------|----------------|-----------------|------------|--------|
| v0.3 | 74.9 | 83.3% | First implementation, greedy clustering | Superseded |
| v0.4 | 95.9 (93.6 sanity) | 96.8% (100% sanity) | Hemisphere histogram rewrite | M1+M2 pass |
| v0.5 | 93.8 | 96.8% | Spatial CC before offset | ABANDONED |
| v0.6 | 94.5 | ~96.8% | Residual-triggered CC | ABANDONED |
| v0.7 | 94.5 | ~96.8% | Adaptive raster cell | Intermediate |
| v0.8 | **96.7** | **100%** | Best params baked in | Intermediate |
| v0.9 | ~96.5 | 83% row_houses | Bbox coplanar merge | ABANDONED |
| **v1.0** | **97.2** | **100%** | Point-proximity coplanar merge | **CHAMPION** |
| v1.1 (Sobel) | 97.2 | 100% | Edge barriers (no effect) | Dead end |
| v1.2 (RegionGrow) | 94.4 | 96.4% | Region growing (regressions) | Niche only |
| v1.3 (Hybrid) | 97.2 | 100% | Height-variance split (no effect) | Dead end |
| DSM-only | 80.3 | 95.2% | Gradient clustering (-17 pts) | Speed-only |
| **v1.4** | **97.2** | **100%** | Expanding-shell fast normals | **CO-CHAMPION** |

## Parameter Grid Sweep Results (21 quick cases)

| Parameter | Value | Avg Composite | Avg matchRate | Notes |
|-----------|-------|---------------|---------------|-------|
| angleThresholdDeg | 5 | 96.3 | 97.6% | Over-segments some normals |
| **angleThresholdDeg** | **8** | **97.2** | **100%** | **Sweet spot** |
| angleThresholdDeg | 10 | 97.1 | 99.4% | Slight under-segmentation |
| angleThresholdDeg | 12 | 96.5 | 99.2% | Row houses merge |
| angleThresholdDeg | 15 | 95.3 | 96.8% | Significant under-segmentation |
| angleThresholdDeg | 20 | 94.5 | 95.6% | Many merges |
| offsetBinSize | 0.05 | 97.1 | 100% | Slightly over-segments |
| **offsetBinSize** | **0.08** | **97.2** | **100%** | **Best** |
| offsetBinSize | 0.10 | 97.1 | 100% | AC unit harder to detect |
| offsetBinSize | 0.15 | 96.2 | 97.6% | Misses fine offset splits |
| offsetBinSize | 0.20 | 96.2 | 97.6% | Too coarse |
| rasterCellSpacingMult | 1.0 | 97.1 | 100% | Slightly finer than needed |
| **rasterCellSpacingMult** | **1.5** | **97.2** | **100%** | **Best** |
| rasterCellSpacingMult | 2.0 | 97.1 | 100% | Still good |
| rasterCellSpacingMult | 3.0 | 96.3 | 99.2% | Misses some spatial splits |
| rasterCellSpacingMult | 4.0 | 96.3 | 99.2% | Too coarse |
| kNeighbors | 8 | 94.3 | 100% | Noisy normals, row_houses drops |
| **kNeighbors** | **15** | **97.2** | **100%** | **Best** |
| kNeighbors | 25 | ~97.2 | -- | Similar to k=15, slower |

**Key insight**: `angleThresholdDeg` is by far the most sensitive parameter. The 8-deg value is uniquely correct for the test suite because it sits exactly between the 7-deg row-house normal difference and the 10+ deg gap where dissimilar planes begin to merge. This is a tight operating window and the most likely parameter to need re-tuning on different building populations.

## All-Synth Benchmark (v1.4, 159 cases)

159 out of 168 potential cases ran (9 loader failures for unsupported medium-size proc types -- all scored 0.0).

### Aggregate Metrics

| Metric | Value |
|--------|-------|
| Total valid cases | 150 (excl. 9 loader failures) |
| Avg composite | **95.1** |
| Avg matchRate | **93.9%** |
| Cases with 100% matchRate | 129/150 (86%) |
| Cases with matchRate < 0.5 | 3 |
| Min composite (valid) | 73.3 (proc-cross_plan-small) |
| Max composite | 98.8 |

### Category Breakdown

| Category | Cases | Avg Composite | Avg matchRate |
|----------|-------|---------------|---------------|
| Pure synth | 87 | 94.8 | 90.1% |
| Procedural (excl. failures) | 63 | 95.4 | 99.3% |

### Per Building Type Performance (avg composite)

**Excellent (>= 98.0)**: flat (98.7), flat_l_shaped (98.7), flat_multi_level_2 (98.7), flat_multi_level_3 (98.7), mono_pitch (98.7), sawtooth (98.7), industrial_stepped_2 (98.7), industrial_stepped_3 (98.7), industrial_l_stepped (98.7), flat_large_commercial (98.6), gable (98.5), shed_lean_to (98.3), asymmetric_gable (98.3), industrial_with_sawtooth (98.2)

**Good (95.0-98.0)**: butterfly (98.0), chalet (98.0), hex_pavilion (97.9), multi_section (97.9), t_shaped (97.7), clerestory (97.7), flat_with_ac_unit (97.6), saltbox (97.6), u_shaped (97.6), hip (97.5), church (97.5), l_shaped (97.3), gambrel (97.3), dormer_house (97.2), cathedral (96.0), flat_large_parapet (96.5), mansard (96.6), courtyard (96.5), octagonal_tower (95.3), hex_tower (94.3), silo_with_barn (95.0), a_frame (95.8), flat_with_high_parapet (95.7)

**Moderate (90.0-95.0)**: mono_pitch_with_parapet (92.6), silo (92.6), split_level (94.1), gable_with_parapet (93.8), row_houses (90.4)

**Weak (< 90.0)**:
- flat_with_antennas: **78.6** -- GT expects 3 planes (roof + 2 antenna pads), antennas have <50 points each
- flat_multi_obstacle: **80.6** -- multiple small obstacles below detection threshold
- cross_plan: **75.6** -- fundamental coplanar bridging (see deep dive below)
- flat_commercial_obstacles: **85.2** -- slightly larger obstacles, partially detected
- hip_with_parapet: **86.2** -- thin vertical parapet strips fragment in raster
- flat_with_vents: **87.0** -- small vent structures with few points
- flat_with_skylight: **88.1** -- skylights share main roof normal

### Failure Categories

**1. Small obstacles (antennas, vents, skylights, commercial obstacles)**: matchRate 0.25-0.57. GT planes have very few points (<50) that fall below the minPlanePoints=8 threshold, or they share normals with the main roof and cannot be separated by the offset histogram (the height difference is within one offset bin). High segAcc (0.97+) shows the main roof is correctly detected; matchRate drops because small GT planes go unmatched.

**2. Cross-plan coplanar bridging**: All 3 cross_plan sizes have ~0.475 segAcc. See deep dive below.

**3. Parapets**: hip_with_parapet at 0.667 matchRate, gable_with_parapet at 0.750. Thin vertical strips with few points. Over-detection (17-18 detected vs 6 GT) suggests fragmentation of parapet geometry into many small components.

## Cross-Plan Deep Dive

### Geometry

A cross-shaped building (two intersecting rectangular wings) with a gable roof on each wing produces **8 GT planes in 4 pairs**. Each pair shares IDENTICAL normals AND offsets (within noise tolerance of 0.002m):

| Pair | Normal | Offset A | Offset B | Spatial Gap |
|------|--------|----------|----------|-------------|
| NS east slopes | (0.809, 0.587, 0) | 6.048 | 6.050 | ~0.5m in Z |
| NS west slopes | (-0.809, 0.587, 0) | 5.417 | 5.414 | ~0.3m in Z |
| EW south slopes | (0, 0.587, 0.809) | 5.669 | 5.668 | ~0.3m in X |
| EW north slopes | (0, 0.587, -0.809) | 5.802 | 5.803 | ~0.3m in X |

### Why the Algorithm Fails

The 2D raster CC (Phase 4) should theoretically split each pair because 0.3-0.5m physical gaps exist in the projected view. However, near the cross junction:
1. Points from one wing have noisy normal estimates (PCA neighborhood includes points from adjacent wing)
2. These misassigned points bridge the gap in the 2D raster projection
3. Two patches that should be separate become one connected component

The offset histogram cannot help (offsets differ by only 0.002m). The coplanar merge cannot help (fragments are already incorrectly merged at Phase 4).

### Attempts to Fix Cross-Plan

| Approach | cross_plan segAcc | Other Regressions | Verdict |
|----------|------------------|-------------------|---------|
| v1.0 baseline | 0.475 | none | Baseline |
| v1.1 Sobel edges | 0.475 | none | No effect (uniform height along normal) |
| v1.2 Region growing | **0.721** | dormer 0.25 match, mansard -4, silo -9 | Trades 4 regressions for 1 fix |
| v1.3 Height-variance | 0.475 | none | No effect (same root cause as Sobel) |

**Conclusion**: Cross-plan is fundamentally hard for any normal-cluster-first approach. The only method that helps (region growing) works by NOT clustering normals globally -- instead growing from local seeds that respect geometric boundaries. But this trades robustness on complex roofs for boundary accuracy at cross-plan junctions.

Notably, `proc-cross_plan_tower` scores much better (95.4-97.0 composite). The tower creates height differences between wings, which the offset histogram can exploit. This confirms: pure coplanarity with only spatial separation is the unsolvable case for this architecture.

## Structural Limitations and Root Causes

### 1. Normal-Cluster-First Ordering

The algorithm commits to normal clusters before considering spatial layout. Points near junction zones (ridges, valleys, wing intersections) have noisy normals that straddle multiple clusters. Once misassigned to a cluster, they act as bridges in the 2D raster CC. This is inherent to the "cluster normals globally, then split spatially" paradigm.

### 2. No Explicit Edge Detection

The pipeline never detects edges (ridges, eaves, valleys) directly. It relies on histogram valleys and raster gaps to infer boundaries indirectly. When two planes meet at a shallow angle or share normals, no explicit boundary detection exists. The original "multi-view edge detection" vision (Hough transform, auto-focus, Bezier fitting) was not implemented.

### 3. Small-Feature Blindness

With minPlanePoints=8 and raster cells of 0.1m+, features under ~50 points often lack enough inliers after filtering. Antennas, vents, and skylights are geometrically valid planes but fall below the detection threshold. Lowering minPlanePoints would introduce noise-plane false positives.

### 4. Metadata Not Exploited

The detector receives only positions and count. LiDAR metadata (scan angles, point source IDs, return numbers) that could enable per-strip rasterization and multi-return edge detection is not available through the harness interface. True multi-view analysis from different sensor passes was untestable.

## Cross-Pollination with Other Research Agents

During the research, the orchestrator facilitated cross-pollination between the four parallel agents:

**Received from Axis 4 (curvature, 97.8 avg)**: Tighter normal angle threshold (10 deg instead of 15 deg) and coplanar plane merging with spatial adjacency check. Both insights were independently discovered by Axis 2 (angle=8 deg and v0.9b proximity merge), confirming convergent evolution across different algorithmic families.

**Shared to Axis 3 (spectral, 93.1 avg)**: The 8-degree angle threshold and fine raster cell (0.1m min, 1.5x spacing) recipe for solving row houses. Axis 3 was also struggling with row_houses at 69.7 composite.

**Blocked by harness limitations**: The orchestrator suggested per-strip rasterization (group by pointSourceId) and multi-return edge seeding, but the harness does not pass metadata to detectors. These ideas remain for future work.

## Configuration Reference

| Parameter | Default | Range Tested | Sensitivity | Notes |
|-----------|---------|-------------|-------------|-------|
| kNeighbors | 15 | 8-25 | Moderate | PCA neighborhood size |
| normalGridRes | 20 | -- | -- | Hemisphere bins = 20x20 = 400 |
| angleThresholdDeg | 8 | 5-20 | **High** | Normal peak merge threshold |
| offsetBinSize | 0.08 | 0.05-0.20 | Moderate | Offset histogram bin width (m) |
| minPlanePoints | 8 | -- | -- | Min points per detected plane |
| minRasterCellSize | 0.1 | -- | Low | Floor for adaptive raster cell (m) |
| rasterCellSpacingMult | 1.5 | 1.0-4.0 | Low | Cell = mult * avgSpacing |
| inlierThreshold | 0.12 | -- | -- | Max point-to-plane distance for inliers |
| assignThreshold | 0.25 | -- | -- | Max distance for unmatched assignment |
| mergeAngleThresh | cos(12 deg) | -- | -- | Coplanar merge normal angle |
| mergeDistThresh | 0.15 | -- | -- | Coplanar merge offset diff |
| mergeProximity | 2.5 * avgSpacing | -- | -- | Coplanar merge point gap limit |

## Future Work

1. **Per-strip rasterization**: If metadata becomes available, rasterize per flight strip and compare per-strip 2D views. Each strip sees the building from a slightly different angle with 2-5cm XY offset. Comparing rasters could reveal ridge lines (strip visibility changes) and eave lines (wall vs roof transitions).

2. **Multi-return edge seeding**: Points with returnNumber < numberOfReturns cluster at physical height discontinuities. In the 2D raster, these would mark eave lines and dormer edges, seeding CC boundary barriers.

3. **Hough transform on 2D projections**: Apply line detection to raster views, back-project detected lines to 3D ridge/eave edges.

4. **Hybrid region-grow + histogram**: Detect cross-plan topology (e.g., via footprint shape analysis), switch to region growing for those specific cases while keeping the normal histogram pipeline for the majority.

5. **Auto-focus optimization**: For each candidate edge direction, rotate the projection plane to maximize line sharpness in the 2D raster.

6. **Harness extension**: Pass LiDAR metadata (scanAngles, pointSourceIds, returnNumbers) through the harness to enable true multi-view analysis.

## Files

| File | Description |
|------|-------------|
| `research/axis-2-multiview/detector.js` | v1.0 champion detector |
| `research/axis-2-multiview/detector-fast.js` | v1.4 speed-optimized co-champion |
| `research/axis-2-multiview/detector-sobel.js` | v1.1 Sobel edge variant (abandoned) |
| `research/axis-2-multiview/detector-regiongrow.js` | v1.2 region growing variant (abandoned) |
| `research/axis-2-multiview/detector-hybrid.js` | v1.3 height-variance variant (abandoned) |
| `research/axis-2-multiview/detector-dsm.js` | DSM gradient variant (abandoned) |
| `research/axis-2-multiview/results/all-synth-v1.4-fast.json` | All-synth benchmark (159 cases) |
| `research/axis-2-multiview/results/grid-*.json` | Parameter sweep results (15 files) |
| `research/axis-2-multiview/results/quick-v*.json` | Per-version quick results (14 files) |
| `research/axis-2-multiview/FINDINGS.md` | Raw chronological findings log |
| `research/axis-2-multiview/MESSAGES.md` | Orchestrator cross-pollination messages |
| `research/axis-2-multiview/PLAN.md` | Original research plan |
