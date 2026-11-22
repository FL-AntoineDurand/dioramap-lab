---
id: "2026-03-06-axis3-spectral-frequency-methods"
date: 2026-03-06
title: "Axis 3: Frequency & Spectral Methods Plane Detection -- Full Research Record"
tags: [research, plane-detection, spectral, graph-laplacian, normal-clustering, agglomerative, offset-histogram]
category: experiment
status: concluded
refs: ["2026-03-06-four-axis-research-synthesis"]
pipeline: null
design_doc: "research/axis-3-spectral/FINDINGS.md"
---

## Summary

Axis 3 set out to apply graph Laplacian spectral segmentation to LiDAR roof
point clouds. The hypothesis was that eigenvectors of the Laplacian on a k-NN
graph would naturally partition roof surfaces at ridges. The hypothesis was wrong.
Pure spectral methods failed fundamentally on roof geometry due to spatial
smoothness across ridges. After two failed spectral attempts, the research pivoted
to agglomerative normal clustering with offset histogram splitting and spatial
connected components -- a deterministic geometric approach that owes nothing to
spectral decomposition. The final champion (v0.7.0) scores 95.1 avg composite on
the 25-case quick set and 91.3 on all-synth (150 loadable cases).

---

## Part 1: The Spectral Failure Story

### 1.1 Motivation and Mathematical Reasoning

The plan was to build a weighted k-NN graph G = (V, E) over the point cloud,
compute the graph Laplacian L = D - W (where D is the degree matrix and W the
adjacency weight matrix), and use its eigenvectors for segmentation. The
mathematical basis:

- The Fiedler vector (eigenvector of the second-smallest eigenvalue of L) gives
  the optimal normalized cut of the graph
- For a mesh with geometric discontinuities (ridges, edges), the Fiedler cut
  should align with those discontinuities
- The eigenvalue gap between consecutive eigenvalues encodes the "natural" number
  of clusters -- a gap after eigenvalue k means k clusters

This is the textbook approach to spectral graph segmentation (Shi & Malik, 2000).
The plan was to use `ml-matrix` for eigendecomposition on subsampled graphs.

### 1.2 v0.1-v0.2: Pure Spatial Graph Laplacian (FAILED)

**Implementation**: Built k-NN graph (k=15) with Gaussian distance weights
w_ij = exp(-||p_i - p_j||^2 / (2 * sigma^2)). Computed full Laplacian
eigendecomposition via `ml-matrix`'s `EigenvalueDecomposition`.

**Performance**: 500x500 matrix eigendecomp ~2s, 800x800 ~12s. O(n^3) scaling
makes anything above ~1000 points impractical for interactive use.

**Result**: Complete failure on gable roof segmentation.

- Fiedler eigenvalue = 0.005 (extremely small, indicating the graph is nearly
  connected everywhere)
- The Fiedler cut goes along the building's **long axis**, not at the ridge
- Reason: a gable roof is a **spatially smooth manifold**. Points near the ridge
  are close to points on both sides. The graph Laplacian optimizes a balanced cut
  through the graph's spatial structure -- and the balanced cut of a rectangle is
  along its length, not its width

**Attempted fix**: Normal-weighted edges. Modified edge weights to
w_ij = exp(-||p_i - p_j||^2 / sigma_s^2) * exp(-angle(n_i, n_j)^2 / sigma_n^2).
Did not help because PCA normal estimation at the ridge produces ambiguous normals
that blend the two planes. The ridge is not a sharp discontinuity in the normal
field when estimated via local PCA on noisy data.

**Conclusion**: Pure graph Laplacian spectral segmentation cannot separate roof
planes because roofs are spatially smooth. The fundamental assumption -- that
geometric discontinuities create high-weight cuts in the spatial graph -- is false
for roof ridges.

### 1.3 v0.3-v0.4: Feature-Space Spectral Clustering (PARTIAL)

**Idea**: Instead of building the graph in pure XYZ space, build it in a
6D feature space: `[x, y, z, scale*nx, scale*ny, scale*nz]` with scale=5-10.
This embeds normal orientation into the graph distance, so points with different
normals are "far apart" even if spatially close.

**Results on clean synthetic data**: Worked. 93-95% split accuracy on clean gable
with scale=8. The Fiedler vector correctly splits the two roof planes because the
normal discontinuity at the ridge creates a gap in feature space.

**Results on actual test cases**: Failed. PCA normal estimation at ridges produces
normals that are averages of both planes (the k=15 neighborhood straddles the
ridge). These blended normals bridge the gap in feature space, causing the Fiedler
vector to produce random splits.

- Fiedler eigenvalue near 0 (graph almost disconnected) but the vector does not
  separate planes correctly
- The feature-space approach only works when normals are **known exactly**, not
  when they are estimated from noisy local neighborhoods

### 1.4 v0.5: Normal-Space Spectral Clustering (PARTIAL)

**Idea**: Skip the spatial graph entirely. Build a graph on the unit normal sphere
(3D), apply spectral bisection there. Then use spatial CC to split each normal
cluster into spatially disconnected components.

**Result**: Conceptually sound. Works, but:
- Eigendecomposition is still O(n^3) and slow
- Ridge normals still blend (same PCA issue)
- No advantage over simpler clustering approaches in normal space

This was the last spectral attempt. The realization at this point was that
eigendecomposition adds computational cost but no segmentation benefit over direct
agglomerative clustering of normals.

### 1.5 The Pivot (10:15 CET)

The key insight that killed the spectral approach and birthed the working one:

> Roof plane segmentation is fundamentally a problem in **normal space**, not
> spatial space. Planes are defined by orientation (normal) and position (offset
> from origin). The correct decomposition is: (1) group by normal direction,
> (2) split by offset within each normal group, (3) split by spatial connectivity
> within each offset group. No eigendecomposition needed.

This is a three-level hierarchical decomposition that provides three orthogonal
separation criteria: orientation, offset, and connectivity. Each level is simple
and deterministic. Together they handle nearly all roof configurations.

---

## Part 2: The Working Approach -- Agglomerative Normal Clustering

### 2.1 v0.6: First Working Version (10:15 CET)

**Architecture** -- seven phases:

1. **Subsample + kNN + PCA normals**: Deterministic stride-based subsampling to
   maxSubsample=500 points. Brute-force kNN (k=15). PCA normal estimation via
   power iteration with deflation (3x3 covariance, no ml-matrix needed).

2. **Agglomerative normal clustering** on the unit sphere:
   - Start: each subsampled point = its own cluster
   - Merge criterion: `cos(angle) = |n_i . n_j|` (absolute value handles sign ambiguity)
   - Stop when: minimum inter-cluster angle > angleThreshold (20 deg in v0.6)
   - Weighted average normal update: `n_merged = (s_I * n_I + sign * s_J * n_J) / ||...||`
   - Max 8 clusters (hard cap, relaxed if angle < 5 deg)

3. **Offset histogram peak finding** within each normal cluster:
   - Project each point onto the cluster's mean normal: `offset = n . p`
   - Build 1D histogram (binWidth=0.3m), smooth with [1,2,1] kernel
   - Find peaks (local maxima), find valleys (local minima between peaks)
   - Split points into groups at valley boundaries

4. **Spatial connected components** within each offset group:
   - Radius = 2x median kNN distance
   - BFS flood-fill, O(n^2) per group

4b. **Coplanar merge** (added in v0.7): Merge segments with angle < 5 deg,
    offset distance < 0.1m, and spatial adjacency < 2m.

5. **Assign ALL points to nearest plane** (not just subsampled):
   - Fit plane to each subsample segment
   - For each of the N full-resolution points, compute point-to-plane distance
     to each fitted plane
   - Assign to closest plane

6. **Iterative refinement** (3 iterations):
   ```
   for iter = 1..3:
     for each segment s: refit plane from all points labeled s
     for each point i: reassign to arg min_s |plane_s . p_i - d_s|
   ```
   This is the **single most impactful technique**. Without it, segmentation
   accuracy is 0.14-0.53 on most cases. With 3 iterations, it reaches 0.92-1.0.
   The refinement works because the initial subsample-based labeling is noisy but
   directionally correct -- iterative refit/reassign converges to clean plane
   boundaries.

7. **Outlier removal**: Points farther than planeDistThreshold (0.3m) from their
   assigned plane are removed.

**Sanity results (v0.6)**:

| Case | Composite | MatchRate | SegAcc | ResidualRMS |
|------|-----------|-----------|--------|-------------|
| noisy-gable | 98.8 | 1.0 | 0.985 | 0.032m |
| perfect-flat | 99.8 | 1.0 | 1.0 | 0m |
| perfect-gable | 76.8 | 1.0 | 0.456 | 0.07m |
| perfect-hip | 99.9 | 1.0 | 1.0 | 0.003m |
| three-parallel-flats | 99.9 | 1.0 | 1.0 | 0.005m |
| **Average** | **95.0** | **100%** | **0.888** | **0.022m** |

The perfect-gable segAcc=0.456 was caused by wall points being assigned to roof
planes. With angleThreshold=20 deg, wall normals (~horizontal) are far enough
from roof normals to form separate clusters, but the plane assignment in Phase 5
pulls wall points into roof segments because they are geometrically close to the
roof planes.

**Quick benchmark results (v0.6, 21/25 loadable)**:

| Case | Composite | MatchRate | SegAcc | Notes |
|------|-----------|-----------|--------|-------|
| gable-default | 98.1 | 1.0 | 0.962 | |
| hip-default | 97.5 | 1.0 | 0.941 | |
| flat-default | 98.5 | 1.0 | 0.978 | |
| mansard-default | 98.0 | 1.0 | 0.961 | |
| l_shaped-default | 96.3 | 1.0 | 0.903 | |
| multi_section | 90.4 | 1.0 | 0.748 | Under-segmented |
| flat_with_ac_unit | 83.4 | 0.5 | 0.976 | Missed AC unit (minSeg=8 too large) |
| proc-cross_plan | 81.8 | 0.875 | 0.553 | Complex multi-facet |
| proc-dormer_house | 77.7 | 0.5 | 0.897 | Dormer surfaces too small |
| proc-row_houses | 69.7 | 0.667 | 0.445 | Same-pitch buildings merged |
| proc-silo | 89.4 | 0.917 | 0.822 | Curved surface approximation |
| **Average (21)** | **93.1** | **92.7%** | **0.870** | |

4 proc cases failed to load (test loader error for `gable`, `hip`, `flat`,
`l_shaped` types -- not detector failures).

### 2.2 M3: Parameter Grid Search

Ran single-variable sweeps over 4 parameters using `run-experiments.sh`:

| Parameter | Values Tested | Best | Avg Composite | Gotcha |
|-----------|--------------|------|---------------|--------|
| normalAngleThreshold | 10, 15, 20, 25, 30 | 15 (93.8) | -- | nat=15 broke noisy-gable sanity (segAcc=0.29) in v0.6 |
| offsetBinWidth | 0.15, 0.2, 0.3, 0.5, 1.0 | 0.2 (93.7) | -- | obw=0.2 over-splits perfect-hip |
| k (kNN) | 8, 10, 15, 20, 25 | 15 (93.1) | -- | Marginal differences across 10-20 |
| planeDistThreshold | 0.1, 0.15, 0.2, 0.3, 0.5 | 0.2-0.3 similar | -- | 0.3 safer for sanity |

**Key finding**: Parameter interactions dominate. Individually optimal values
break sanity cases. obw=0.2 is better on quick but over-splits the offset
histogram within normal clusters on hip roofs (4 hip planes have similar offsets).
nat<20 separates more normals but noise creates >20 deg normal variation, breaking
noisy-gable. Must keep nat=20, obw=0.3 for v0.6 safety.

### 2.3 v0.7.0: The Improvements That Worked

Three changes from v0.6 to v0.7:

**1. minSegmentSize 8 -> 5**: Dramatically improved detection of small surfaces.
The AC unit on `flat_with_ac_unit` was being filtered out because its subsample
cluster had 6-7 points (below the threshold of 8). Dropping to 5 enabled
detection. Result: flat_with_ac_unit 83.4 -> 96.6 (+13.2).

**2. Conservative coplanar merge**: `mergeCoplanarSegments()` merges segments
with angle < 5 deg, offset distance < 0.1m, and spatial adjacency within 2m.
Initial attempt with 12 deg/0.2m caused severe regression on perfect-hip (merged
distinct hip planes that happen to be close in offset space). Conservative 5/0.1m
is safe. Fixes minor over-segmentation on cross_plan and multi_section.

**3. Spatial-weighted plane assignment**: In Phase 5, the assignment score becomes:
```
score(point, segment) = |plane.n . point - plane.d| + spatialWeight * ||point_xy - centroid_xy|| / maxSpan
```
with spatialWeight=0.1. This adds a small penalty for assigning a point to a
spatially distant segment, helping distinguish between parallel planes in different
spatial locations. Result: multi_section +3.4, cross_plan +3.2, avg +0.2.

**4. nat=15 now safe** (discovered during M4): With spatial-weighted assignment
in Phase 5, the over-segmentation from finer normal threshold (15 vs 20 deg) is
compensated by the spatial proximity weight during assignment. noisy-gable sanity
case went from segAcc=0.29 (broken with nat=15 in v0.6) to 0.985 (safe in v0.7).
Also adopted planeDistThreshold=0.25 (from 0.3) -- same composite, better residualRMS.

**Sanity results (v0.7.0)**:

| Case | v0.6 | v0.7 | Delta |
|------|------|------|-------|
| noisy-gable | 98.8 | 98.8 | 0 |
| perfect-flat | 99.8 | 99.8 | 0 |
| perfect-gable | 76.8 | 100.0 | **+23.2** |
| perfect-hip | 99.9 | 99.9 | 0 |
| three-parallel-flats | 99.9 | 99.9 | 0 |
| **Average** | **95.0** | **99.7** | **+4.7** |

The perfect-gable fix (76.8 -> 100.0) came from minSegmentSize=5 allowing the
wall cluster to form properly as a small segment that gets filtered in Phase 7.

### 2.4 M4: Abandoned Experiments

Seven variations attempted, one adopted:

**1. Planarity-weighted normal clustering** (ABANDONED)
- Idea: Exclude low-planarity (ridge/edge) points from initial clustering, assign
  them after plane fitting. Points with planarity < 0.3 would be deferred.
- Result: **-9 on mansard** (quick avg). Mansard roofs have many non-planar
  transition points between steep and flat sections. Excluding them guts the
  clustering, producing fragmented segments.
- Verdict: Curvature features help in theory but mansard is the deal-breaker.

**2. Raster-based spatial CC** (ABANDONED)
- Idea: Replace radius-based BFS CC (O(n^2)) with 2D grid projection + flood fill.
- Result: **-0.5 avg**. Grid cell size is hard to tune -- too large merges
  different components, too small fragments them. The radius-based approach adapts
  to local point density via the median kNN distance.
- Verdict: Marginal performance difference, harder to tune.

**3. Adaptive subsampling** (ABANDONED)
- Idea: Scale maxSubsample up to 1200 for larger buildings.
- Result: **-0.7 avg**. More subsample points create more fine-grained initial
  clusters that fragment during agglomerative merging. The refinement phase (which
  operates on ALL points) is what actually determines final quality -- the
  subsample only needs to be good enough for initial seeding.
- Verdict: 500 subsamples is a sweet spot.

**4. Spatial-weighted plane assignment** (ADOPTED -- see 2.3 above)
- Result: **+0.2 avg**, multi_section +3.4, cross_plan +3.2.

**5. Spatial-weighted iterative refinement** (ABANDONED)
- Idea: Apply the same spatialWeight during Phase 6 refinement (not just Phase 5
  initial assignment).
- Result: **-0.4 avg**. Destabilizes convergence. The refinement needs pure
  plane-distance to converge to clean boundaries. Adding spatial bias causes
  oscillation between competing assignments.
- Verdict: Spatial weighting helps initial assignment but hurts refinement.

**6. Post-refinement grid split** (ABANDONED -- available as opt-in)
- Idea: After Phase 6 refinement, apply grid-based CC (cellSize=0.5m) on
  full-resolution points to split spatially disconnected segments.
- Result: row_houses **+3.8** (matchRate 0.667 -> 0.833), but flat_with_ac_unit
  **-14.1** (the AC unit gets split from its base plane). Available as
  `splitCellSize` parameter but disabled by default.
- Verdict: Helps one case, destroys another. Not worth the trade-off.

**7. 1D gap detection** (ABANDONED)
- Idea: Within each segment, project points along the longest principal axis.
  Split at gaps > 3x median inter-point distance (or > 0.5m absolute).
- Result at 3x/0.5m: row_houses **+12.8** (matchRate -> 1.000!), but l_shaped
  **-9.0**, flat_multi_level_3 **-10.8**. The gap at the junction of an L-shaped
  building looks identical to the gap between row houses.
- Result at 5x/1.0m: No benefit (gaps between row houses are smaller than 1m).
- Verdict: **Fundamental ambiguity** -- there is no reliable threshold that
  separates "gap between buildings" from "junction within a building". This is the
  core of the row_houses problem.

---

## Part 3: Final Results

### 3.1 Score Progression Table

| Version | Quick Composite | Quick MatchRate | Sanity Avg | Key Changes |
|---------|----------------|-----------------|------------|-------------|
| v0.1-v0.2 | -- | -- | -- | Pure graph Laplacian: FAILED (O(n^3), Fiedler cut wrong) |
| v0.3-v0.4 | -- | -- | -- | Feature-space spectral: PARTIAL (clean data only) |
| v0.5 | -- | -- | -- | Normal-space spectral: works but no advantage over simple clustering |
| v0.6 | 93.1 | 92.7% | 95.0 | Normal clustering + offset histogram + spatial CC + iterative refinement |
| v0.7 (nat=20) | 94.1 | 95.6% | 99.7 | + minSegSize=5, coplanar merge |
| **v0.7 (nat=15)** | **95.1** | **96.8%** | **99.7** | + spatialWeight=0.1, nat=15, pdt=0.25 |

### 3.2 All-Synth Results (150 loadable / 159 cases)

| Metric | v0.6 | v0.7 final |
|--------|------|------------|
| Avg Composite | ~90 | 91.3 |
| Avg MatchRate | ~84% | 86.2% |
| Pass Rate (matchRate >= 0.5) | ~89% | 90.7% |

### 3.3 Quick Set: Per-Case Final Scores (v0.7.0)

| Case | Composite | MatchRate | SegAcc | v0.6 Delta |
|------|-----------|-----------|--------|------------|
| gable-default | 98.0 | 1.0 | 0.961 | -0.1 |
| hip-default | 97.8 | 1.0 | 0.952 | +0.3 |
| flat-default | 98.4 | 1.0 | 0.974 | -0.1 |
| flat-large | 98.7 | 1.0 | 0.982 | 0 |
| asymmetric_gable | 98.4 | 1.0 | 0.974 | -0.1 |
| mansard | 97.8 | 1.0 | 0.954 | -0.2 |
| mono_pitch | 98.2 | 1.0 | 0.966 | -0.2 |
| l_shaped | 97.3 | 1.0 | 0.935 | +1.0 |
| flat_multi_level_2 | 98.7 | 1.0 | 0.982 | +0.2 |
| flat_multi_level_3 | 97.3 | 1.0 | 0.936 | **+4.5** |
| flat_l_shaped | 98.5 | 1.0 | 0.977 | -0.2 |
| multi_section | 93.8 | 1.0 | 0.835 | **+3.4** |
| industrial_stepped_2 | 98.6 | 1.0 | 0.979 | +0.6 |
| flat_with_ac_unit | 97.5 | 1.0 | 0.974 | **+14.1** |
| proc-cross_plan | 84.3 | 1.0 | 0.514 | +2.5 |
| proc-dormer_house | 78.2 | 0.5 | 0.885 | +0.5 |
| proc-gambrel | 97.3 | 1.0 | 0.933 | **+3.4** |
| proc-saltbox | 98.3 | 1.0 | 0.967 | +0.2 |
| proc-butterfly | 98.3 | 1.0 | 0.969 | 0 |
| proc-row_houses | 79.1 | 0.833 | 0.567 | **+9.4** |
| proc-silo | 93.4 | 1.0 | 0.820 | +4.0 |

Best improvements: flat_with_ac_unit (+14.1), row_houses (+9.4),
flat_multi_level_3 (+4.5), silo (+4.0).

Weakest cases remaining: proc-row_houses (79.1), proc-dormer_house (78.2),
proc-cross_plan (84.3).

### 3.4 Champion Final Configuration

```javascript
k = 15                       // kNN neighbors for spatial graph + normal estimation
kNormal = 12                 // neighbors used for PCA normal estimation
maxSubsample = 500           // deterministic stride-based subsampling
minSegmentSize = 5           // minimum points per segment (lowered from 8)
maxNormalClusters = 8        // hard cap on agglomerative clustering
normalAngleThreshold = 15    // degrees; merge normals within this angle
offsetBinWidth = 0.3         // meters; 1D histogram bin width for offset splitting
spatialRadiusFactor = 2.0    // spatial CC radius = 2x median kNN distance
planeDistThreshold = 0.25    // meters; outlier removal threshold
mergeAngle = 5               // degrees; coplanar merge angle threshold
mergeDistThreshold = 0.1     // meters; coplanar merge offset distance threshold
spatialWeight = 0.1          // weight for spatial distance in plane assignment
refineIters = 3              // iterative refinement rounds
```

---

## Part 4: Key Insights and Structural Limitations

### 4.1 The Iterative Refinement Discovery

The single most impactful technique. The problem: Phase 2-4 operate on 500
subsampled points and produce rough clusters. When Phase 5 assigns all N points
to the nearest plane, boundary regions are noisy because the planes were fitted
on sparse subsamples.

The solution: iteratively refit planes from ALL assigned points, then reassign
ALL points to the refitted planes. Each iteration sharpens boundaries because:
- More points per plane = better plane fit
- Better plane fit = more accurate point-to-plane distances
- More accurate distances = better assignments
- Better assignments = even more accurate plane fit (next iteration)

Convergence is fast: 3 iterations suffice. Without refinement, segAcc ranges from
0.14 to 0.53 across test cases. With 3 iterations, it reaches 0.92-1.0. This is
a 2-5x improvement in segmentation accuracy from a simple iterative procedure.

### 4.2 Normal-Space vs Spatial-Space Decomposition

The core insight that distinguishes the working approach from the failed spectral
attempts: roof planes are defined by their **normal direction**, not their spatial
position. Two points on the same plane can be spatially far apart but share the
same normal. Two points on different planes can be spatially adjacent but have
different normals.

The graph Laplacian operates in spatial space and finds cuts that minimize edge
weight across the cut -- which for a smooth manifold like a roof is the long-axis
bisection, not the ridge. Normal clustering operates in the correct space: the
unit sphere of normal directions.

The offset histogram adds the second dimension: parallel planes (same normal,
different height) are separated by their dot product with the shared normal
direction, which is a 1D problem.

### 4.3 The Row Houses Limitation

Row houses (same-pitch gable buildings adjacent to each other) remain the weakest
case (79.1 composite, 0.833 matchRate). The fundamental issue:

1. All gable planes across N row houses have the **same two normal directions**
2. Normal clustering merges them into 2 clusters (left-facing, right-facing)
3. The offset histogram can sometimes separate them (different heights) but
   often cannot (same height row houses)
4. Spatial CC can separate them if there is a physical gap, but the gap between
   row houses in the test data is small relative to the spatial radius

Attempted fixes and why they fail:
- **Gap detection** (1D projection split): separates row houses (+12.8) but also
  splits L-shaped buildings at their junction (-9.0). The gap signature is
  ambiguous.
- **Post-refinement grid split**: separates some cases (+3.8) but fragments
  small features like AC units (-14.1).
- **Finer spatial CC radius**: would help row houses but fragments single
  buildings with sparse point coverage.

This is a fundamental limitation of normal-space decomposition. Without external
information (e.g., building footprints from BDTOPO), same-pitch multi-building
configurations cannot be reliably distinguished from single multi-wing buildings.

### 4.4 Comparison with V6 Normal Cascade

The main codebase's V6 normal cascade detector uses histogram binning of normals
(not agglomerative clustering). Key differences:

| Aspect | Axis 3 (agglomerative) | V6 (histogram) |
|--------|----------------------|-----------------|
| Normal grouping | Pairwise merge on unit sphere | 2D histogram of (azimuth, elevation) |
| Cluster count | Determined by angle threshold + merge stopping | Determined by peak finding in 2D histogram |
| Offset separation | 1D histogram per normal cluster | 1D histogram per normal bin |
| Spatial splitting | Radius-based BFS CC | Raster-based CC |
| Iterative refinement | 3 rounds of refit+reassign | Not present (single-pass) |
| Row houses | 79.1 | Similar limitation |

The agglomerative approach is more principled (operates directly on angular
distances) but V6's histogram binning is faster for large point counts.

---

## Part 5: Files and Artifacts

| File | Description |
|------|-------------|
| `research/axis-3-spectral/detector.js` | Champion detector (v0.7.0, 730 lines) |
| `research/axis-3-spectral/FINDINGS.md` | Chronological findings document |
| `research/axis-3-spectral/PLAN.md` | Original research plan |
| `research/axis-3-spectral/MESSAGES.md` | Orchestrator communications log |
| `research/axis-3-spectral/run-experiments.sh` | M3 parameter grid search script |
| `research/axis-3-spectral/results/quick-v06.json` | v0.6 quick benchmark (25 cases) |
| `research/axis-3-spectral/results/quick-v07.json` | v0.7 first quick run |
| `research/axis-3-spectral/results/quick-v07-nat15.json` | v0.7 with nat=15 |
| `research/axis-3-spectral/results/quick-v07-spatweight.json` | v0.7 with spatial weighting |
| `research/axis-3-spectral/results/quick-v07-final2.json` | v0.7 final (champion) |
| `research/axis-3-spectral/results/nat-*.json` | Grid search: normalAngleThreshold |
| `research/axis-3-spectral/results/obw-*.json` | Grid search: offsetBinWidth |
| `research/axis-3-spectral/results/k-*.json` | Grid search: k (kNN neighbors) |
| `research/axis-3-spectral/results/pdt-*.json` | Grid search: planeDistThreshold |

No all-synth JSON was saved (the 91.3 score was from in-memory computation during
the benchmarking session).

---

## Part 6: Lessons for Future Work

1. **Spectral methods are the wrong tool for plane detection.** The graph
   Laplacian finds globally optimal cuts in the connectivity structure, not in
   the geometric structure. For planar segmentation, direct analysis of normal
   distributions is both faster and more accurate.

2. **Iterative refinement is free performance.** The refit/reassign loop costs
   O(N * K) per iteration (N points, K planes) and provides 2-5x segmentation
   accuracy improvement. Any plane detection approach should include this.

3. **Parameter interactions dominate single-parameter optimization.** Grid search
   on individual parameters misses that reducing angleThreshold requires spatial
   weighting to compensate for over-segmentation. Multi-parameter joint
   optimization or principled parameter derivation (from sensor physics) is needed.

4. **The 500-point subsample is sufficient.** Increasing subsample size hurts
   rather than helps. The subsample only needs to capture the major normal
   directions -- the iterative refinement on full-resolution points handles the
   rest.

5. **Row houses need external information.** No point-cloud-only approach can
   reliably distinguish "two adjacent same-pitch buildings" from "one L-shaped
   building". Building footprints from BDTOPO or cadastral data would resolve
   this ambiguity.
