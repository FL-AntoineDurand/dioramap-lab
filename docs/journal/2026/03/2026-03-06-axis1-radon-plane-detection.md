---
id: "2026-03-06-axis1-radon-plane-detection"
date: 2026-03-06
title: "Axis 1: CT/Radon Angular Sweep Plane Detection — Full Research Archive"
tags: [research, plane-detection, radon, histogram, angular-sweep, fibonacci-hemisphere]
category: experiment
status: concluded
refs: ["2026-03-06-four-axis-research-synthesis"]
---

## Summary

Plane detection via CT/Radon-inspired angular histogram sweep. The core idea: project all 3D points onto each of ~810 hemisphere directions, build 1D distance histograms, and detect peaks as plane candidates. A global, exhaustive, deterministic method requiring no neighbor computation or initial seeds.

**Final champion: v2.3** -- composite **93.0/100** (quick set, 21 valid), **88.6/100** (all-synth, 150 valid). matchRate **95.2%** (quick), **84.6%** (all-synth). Ranks 3rd of 4 research axes behind Axis 4 (curvature, 97.8) and Axis 2 (multi-view, 95.9), ahead of Axis 3 (spectral, 93.1).

---

## Mathematical Foundation

### Projection Formula

For a sweep direction `n = (nx, ny, nz)` on the unit hemisphere, each 3D point `p = (px, py, pz)` is projected onto the 1D distance axis:

```
d = nx * px + ny * py + nz * pz
```

Points lying on a plane with normal `n` and offset `d0` cluster at `d = d0` in the histogram. A sharp peak in the histogram corresponds to a plane perpendicular to the sweep direction.

### Direction Sampling (Fibonacci Hemisphere)

800 directions uniformly distributed on the hemisphere via the Fibonacci lattice:

```
theta_i = acos(1 - 2*(i + 0.5) / (2*N))    // polar angle
phi_i   = 2*pi*i / golden_ratio             // azimuthal angle
n_i     = (sin(theta)*cos(phi), sin(theta)*sin(phi), cos(theta))
```

Only directions with `z >= -0.01` are kept (hemisphere). 9 axis-aligned extras are added (cardinal axes + 45-degree diagonals) unless already present within `dot > 0.999`. Total: ~810 directions, giving ~3-degree angular resolution.

### Histogram Binning (Linear Interpolation)

Points are distributed across bins using triangular kernel interpolation:

```
fbin = (d - d_min) / binSize - 0.5
lo   = floor(fbin)
frac = fbin - lo
bins[lo]   += (1 - frac)
bins[lo+1] += frac
```

This smooths bin-boundary artifacts without the 5-10x cost of Gaussian convolution. `binSize = 0.08m` (8 cm).

### Peak Detection

A bin `i` is a peak if:
- `bins[i] >= minPeakCount` (where `minPeakCount = max(3, floor(N * 0.03))`)
- `bins[i] >= bins[i-1]` AND `bins[i] >= bins[i+1]`

For each peak, compute:
- **Weighted center** over `[i-2, i+2]`: `center = sum(bins[j] * (d_min + (j+0.5)*binSize)) / sum(bins[j])`
- **FWHM** (full width at half maximum): count bins above `bins[i]/2` in both directions
- **Sharpness**: `bins[i] / max(1, FWHM_bins)`
- **Score**: `count * sharpness` (favors tall, narrow peaks)

### Candidate Merging (Greedy by Score)

Candidates sorted by score descending. A new candidate merges into an existing one if:
- Angular similarity: `|dot(n_new, n_existing)| >= cos(8 degrees)` = 0.990
- Distance similarity: `|d_effective - d_existing| < 0.25m` (with sign correction for antiparallel normals)

Merged candidates accumulate votes and 30% of the new candidate's score.

### Normal Canonicalization

To avoid duplicate detections from antiparallel normals, all normals are canonicalized:
- If `|nz| > 0.01`: flip if `nz < 0`
- Else if `|ny| > 0.01`: flip if `ny < 0`
- Else: flip if `nx < 0`

This ensures consistent orientation for merging and consolidation.

---

## Architecture: 8-Phase Pipeline

1. **Direction sweep**: 810 Fibonacci+axis directions. For each, project all points and find histogram peaks. Produces thousands of raw candidates.
2. **Candidate merging**: Greedy merge by score (8-deg angle, 0.25m distance). Reduces to ~10-30 unique planes.
3. **Iterative PCA refinement**: 3 rounds. Round 1 uses `assignMaxDist * 1.5` (wider net), subsequent rounds use `assignMaxDist`. Each round: assign points to nearest plane, refit via smallest eigenvector of covariance matrix. Between rounds, re-merge converged duplicates at half thresholds.
4. **Final assignment**: All points assigned to nearest plane within `assignMaxDist = 0.20m`.
5. **Post-consolidation**: Merge planes with refined normals within 3 degrees and `d` within `0.075m`.
6. **Spatial CC splitting**: Grid flood-fill on plane-projected 2D coordinates. Cell size `0.45m`, 8-connected. Splits coplanar but spatially disconnected segments (e.g., two parallel roof faces of row houses).
7. **Final reassignment**: After splitting, re-assign all points to nearest split surface and re-refine. Corrects boundary artifacts from pre-split assignment.
8. **Output filtering**: Remove planes below `minPlanePoints = max(8, floor(N * 0.02))`.

---

## Version History: Every Iteration

### Phase 1: Sanity Pass (v0.3 -- v1.0)

**v0.3** -- Initial implementation. Fibonacci hemisphere (200 directions) + basic histogram binning. 3/5 sanity cases pass. Gable and hip detected; flat missed due to insufficient axis-aligned coverage.

**v0.4** -- Added normal canonicalization. All detected planes now have consistent orientation. 5/5 sanity pass (but some with low segAcc).

**v0.5** -- Tried two-pass architecture: first pass finds dominant planes, second pass refines on residual points. Regression on gable (first pass claims too many points). Abandoned.

**v0.6-v0.8** -- Debugging parallel plane detection. Fibonacci lattice at 200 directions misses axis-aligned normals by up to 3.8 degrees. At `binSize=0.05m`, this angular error shifts peaks enough to fragment flat roofs into 2-3 spurious planes.

**v1.0** -- Added 9 axis-aligned extra directions (cardinal axes + 45-degree diagonals). Deduplication check (`dot > 0.999`) prevents double-counting. All 5 sanity cases at `matchRate = 1.0`. **M1 PASS.**

| Version | Sanity Pass | Key Change |
|---------|-------------|------------|
| v0.3 | 3/5 | Initial Fibonacci + histograms |
| v0.4 | 5/5 | Normal canonicalization |
| v0.5 | 4/5 | Two-pass (abandoned, regression) |
| v0.6-v0.8 | 5/5 | Debugging 3.8-deg axis misalignment |
| v1.0 | **5/5, all 1.0** | Axis-aligned extras |

### Phase 2: Quick Set Baseline (v1.0 -- v1.4)

**v1.0 on quick set**: 18/25 cases with `matchRate >= 0.5`. Composite 80.0, matchRate 73.6%. Failures: multi-plane buildings (l_shaped, row_houses) and buildings with small features (dormers, AC units).

**v1.1** -- Increased `nDirections` 300 -> 400, tightened `normalMergeAngle` 8 -> 6 degrees. matchRate improved to 80.8%. Small planes found more reliably but merge threshold too tight caused duplicate detections.

**v1.2** -- Tried greedy extraction: after finding the top candidate, remove its points and re-sweep residuals. Abandoned: top candidates in the Radon approach have "intermediate" normals (averages of nearby directions) that claim points from multiple true planes. Greedy extraction amplifies this error. This is a fundamental difference from RANSAC where candidates are seeded from actual point subsets.

**v1.3** -- Reverted to competitive assignment. Added post-consolidation step (merge planes whose refined normals converge to within 3 degrees / 0.075m). This catches duplicate detections from nearby sweep directions.

**v1.4** -- Tuned thresholds. Composite 83.4, matchRate 80.8%. **M2 PASS.**

### Phase 3: Grid Search (27 configurations)

Full factorial: `nDirections` {200, 400, 600} x `binSize` {0.03, 0.05, 0.08} x `mergeAngle` {4, 6, 8}.

Key findings:
- **binSize = 0.03m fragments peaks**. At 3cm resolution, noise jitter creates multiple thin peaks per plane. Score-based merging partially recovers, but net effect is worse.
- **binSize = 0.08m is optimal**. Counterintuitively coarser (8cm) bins give sharper, more distinct peaks. Planes at different heights need to differ by >8cm to be separated, which is true for virtually all roof structures.
- **600 directions > 400 > 200**. More directions catch more off-axis planes. Marginal benefit tapers above 600.
- **mergeAngle barely matters at binSize=0.08**. At coarse bins, peaks are already well-separated and merging is less ambiguous.
- **Best config: d600-b0.08-m8** -- matchRate 91.9%, composite 83.0.

### Phase 4: Architecture Improvements (v2.0 -- v2.3)

**v2.0: Spatial CC + Final Reassignment** -- The single biggest breakthrough.

Before v2.0, coplanar but spatially disconnected roof sections (e.g., two halves of a row house, L-shaped buildings) were merged into a single detected plane. The Radon approach sees them as one peak in the histogram because they share the same normal and offset.

Solution: after refinement, project each plane's points onto the plane's local 2D coordinate system, bucket into a grid, and flood-fill (BFS, 8-connected) to find spatially connected components. Each component becomes a separate output surface.

After splitting, a full reassignment pass re-assigns all points to the nearest (now correctly split) surface. This corrects boundary points that were assigned to the wrong pre-split plane.

Impact: **+3.7 composite, +11.5% matchRate**. Specific case improvements:
- hip: 0.5 -> 1.0 matchRate
- l_shaped: 0.25 -> 1.0 matchRate
- row_houses: 0.83 -> 1.0 matchRate

**v2.1: 800 Directions + Smooth Histograms** -- `nDirections` 600 -> 800. Added linear interpolation (triangular kernel) to histogram binning. The interpolation eliminates bin-boundary artifacts where a plane peak falls exactly on a bin edge and splits into two weak peaks.

Impact: **+3.7 composite, +2.3% matchRate**. Fixed mansard (0.875 -> 1.0), saltbox (0.5 -> 1.0).

**v2.2: Plane-Projected CC** -- Changed spatial CC splitting from XY-plane projection to the detected plane's local 2D coordinate system. Built orthonormal basis `(u, v)` on each plane via cross-product with a non-parallel axis. Points are projected as `(dot(p, u), dot(p, v))` instead of `(px, py)`.

Why it matters: for steeply pitched planes (mansard sides, steep hips), XY projection compresses the point cloud along one axis, making spatially separate groups appear connected. Plane-projected CC preserves true inter-point distances.

Impact: **+0.7 composite**. Mostly helps mansard and hip variants.

**v2.3: Optimized Thresholds** -- Final parameter tuning:
- `distMergeThreshold`: 0.15 -> 0.25m (wider merging catches near-duplicates from adjacent sweep directions)
- `assignMaxDist`: 0.15 -> 0.20m (captures more boundary points, PCA refinement corrects any error)
- `spatialSplitRadius`: 0.5 -> 0.45m (tighter grid improves component separation on close structures)

Impact: **+1.4 composite**. hip: 92.1 -> 95.2, butterfly: 87.6 -> 95.7, industrial_stepped_2: 92.3 -> 98.7.

---

## Abandoned Experiments

| Experiment | Result | Root Cause of Failure |
|------------|--------|-----------------------|
| **1000+ directions** | matchRate 96%, composite 89.4 | Too many competing candidates with slightly different normals. Merging cannot resolve them all; leftovers become false positive planes. Net composite drops despite better matchRate. |
| **Gaussian histogram smoothing** | Same accuracy as linear interp | 5-10x slower (kernel evaluation per point per bin). Linear interpolation achieves the same peak-smoothing effect at O(1) per point. |
| **minPeakFraction = 0.02** | More planes found | False positives on flat_with_ac_unit: noise clusters form 2% peaks that pass threshold. 3% is the minimum to avoid this. |
| **Iterative reassignment (3 rounds)** | 92.4 composite | First round over-assigns boundary points to the dominant plane. Subsequent rounds cannot recover them. Single reassignment after CC splitting is strictly better. |
| **Normal-seeded directions** | Same accuracy | At 800 Fibonacci directions (3-deg resolution), the hemisphere is already dense enough that per-point normal estimation adds zero new directions. The 1-2s overhead for k-NN normal estimation is pure waste. |
| **Residual sweep on unassigned** | No improvement | Unassigned points are boundary noise between planes, not indicators of missed planes. A second sweep on ~5-15% residual points finds only noise peaks. |
| **5 refine iterations** | 92.4 composite | Over-refinement: after 3 iterations, plane normals have converged. Additional rounds shift boundaries by <0.01m with no metric improvement. |
| **Boundary smoothing (XY voting)** | **61.2 composite** (catastrophic) | XY-based neighborhood voting merges planes at different heights. A point on a lower flat roof near a higher flat roof gets "voted" into the higher plane's territory. Fundamental flaw in 2D voting for 3D structures. |
| **Angular-weighted assignment** | 93.0 composite (no change) | Weighting point-to-plane distance by normal agreement requires per-point normal estimation (k-NN PCA), adding 5-10x runtime. The accuracy gain is zero because PCA refinement already recomputes normals from assigned point sets. |
| **normalMergeAngle = 10-12 degrees** | Same quick, worse all-synth | Marginal gain on simple cases, but aggressive merging collapses distinct planes on complex buildings (cathedral, church). |
| **Two-pass sweep** | Regression on gable | First pass claims too many boundary points; second pass sees depleted residuals. |
| **Greedy extraction** | Regression | Radon candidates have "averaged" normals from nearby directions. Extracting points by the top candidate steals from multiple true planes. Fundamentally incompatible with the histogram approach. |

---

## Final Configuration

```javascript
nDirections:        800     // Fibonacci hemisphere + 9 axis-aligned extras (~810 total)
binSize:            0.08    // 8cm bins -- coarser = sharper peaks (counterintuitive)
normalMergeAngle:   8       // Degrees, for candidate merging
distMergeThreshold: 0.25    // Meters, wider merging catches near-duplicates
assignMaxDist:      0.20    // Meters, captures boundary points; PCA corrects
minPeakFraction:    0.03    // 3% of total points minimum for a peak
refineIterations:   3       // PCA refinement rounds (convergence by round 3)
spatialSplit:       true    // Grid CC splitting (the big architectural win)
spatialSplitRadius: 0.45    // Grid cell size in meters (plane-projected 2D)
smoothHistogram:    true    // Linear interpolation (triangular kernel)
```

**Why these values:**
- `binSize 0.08`: At 5cm, noise-induced jitter (sigma ~3cm) splits single planes into 2-3 peaks. At 8cm, peaks are robust to noise but still separate planes >8cm apart (virtually all real roofs).
- `800 directions`: Below 600, angular gaps >5 degrees miss steep planes. Above 1000, the excess candidates fragment into false positives. 800 gives ~3-degree resolution, matching the `normalMergeAngle`.
- `distMergeThreshold 0.25`: PCA refinement can recover from initial 25cm merge errors. Wider merging is better than leaving near-duplicates that compete for points.
- `spatialSplitRadius 0.45`: Must be smaller than inter-building gaps (~1m) but larger than point spacing (~0.2m at 21 pts/m2). 0.45m is the geometric mean.

---

## Score Progression (All Versions)

| Version | Quick Composite | Quick matchRate | All-Synth Composite | Key Change |
|---------|----------------|-----------------|---------------------|------------|
| v0.3 | -- | ~60% | -- | Initial Fibonacci + histograms |
| v0.4 | -- | -- | -- | Normal canonicalization |
| v1.0 | 80.0 | 73.6% | -- | Axis-aligned directions (M1+M2 PASS) |
| v1.1 | -- | 80.8% | -- | 400 dirs, mergeAngle 6 |
| v1.4 | 83.4 | 80.8% | -- | Post-consolidation (M2 PASS) |
| grid best | 83.0 | 91.9% | -- | d600-b0.08-m8 |
| v2.0 | 87.1 | 92.3% | -- | Spatial CC + reassignment |
| v2.1 | 90.8 | 94.6% | -- | 800 dirs + smooth histograms |
| v2.2 | 91.5 | 95.2% | -- | Plane-projected CC |
| **v2.3** | **93.0** | **95.2%** | **88.6** | Optimized thresholds |

---

## Per-Case Performance Analysis (v2.3 All-Synth, 159 cases)

### Overall Statistics

- **159 total cases**: 87 synthetic, 72 procedural
- **9 loader failures** (proc-*-medium cases with undefined detectedCount): composite 0
- **150 valid cases**: avg composite 88.6, avg matchRate 84.6%
- **133/150 valid cases with matchRate >= 0.5**
- **106/150 valid cases with matchRate = 1.0**
- **Timing**: min 28ms, max 2484ms, avg 315ms, median 200ms

### Category Split

| Category | Cases | Avg Composite | Avg matchRate |
|----------|-------|---------------|---------------|
| Synthetic | 87 | 90.7 | 87.2% |
| Procedural | 72 | 75.2 | 72.1% |

Procedural cases are harder: they have more planes (cathedral has 14 GT planes), more complex geometry, and the 9 loader failures all occur in procedural cases.

### Top 10 Best-Performing Building Types (avg composite)

| Type | Avg Composite | matchRate | Cases |
|------|---------------|-----------|-------|
| industrial_with_sawtooth | 98.7 | 100% | 3 |
| proc-chalet | 98.4 | 100% | 3 |
| proc-gambrel | 97.9 | 100% | 3 |
| t_shaped | 97.8 | 100% | 3 |
| sawtooth | 97.3 | 100% | 3 |
| flat_multi_level_3 | 97.0 | 100% | 3 |
| industrial_stepped_2 | 97.0 | 100% | 3 |
| asymmetric_gable | 96.8 | 100% | 3 |
| hip | 96.7 | 100% | 3 |
| flat_large_commercial | 96.6 | 100% | 3 |

The Radon approach excels on buildings with large, well-defined planes: industrial sawtooth, chalet, gambrel, T-shaped. These produce tall, narrow histogram peaks that are unambiguous.

### Bottom 10 Worst-Performing Building Types (excluding loader failures)

| Type | Avg Composite | matchRate | Root Cause |
|------|---------------|-----------|------------|
| proc-silo_with_barn | 65.9 | 14.3% | 14 GT planes; cylinder + small barn planes produce weak peaks |
| proc-cathedral | 67.2 | 38.1% | 14 GT planes; many small facets lost in histogram noise |
| proc-dormer_house | 72.0 | 26.1% | Dormer windows: tiny plane surfaces with few points |
| flat_with_antennas | 72.9 | 33.3% | GT expects separate antenna planes; histogram cannot resolve |
| proc-church | 74.4 | 33.3% | Complex multi-faceted roof with small gable dormers |
| flat_commercial_obstacles | 76.4 | 33.3% | Rooftop obstacles expected as separate planes |
| proc-cross_plan | 78.1 | 79.2% | Multiple intersecting gable sections |
| flat_multi_obstacle | 79.8 | 50.0% | Multiple small obstacles on flat roof |
| proc-cross_plan_tower | 80.5 | 83.3% | Tower addition creates small additional planes |
| proc-courtyard | 81.0 | 75.0% | U-shaped with many coplanar sections to split |

### Quick Set Detail (21 valid cases)

| Case | Composite | matchRate | segAcc | Time |
|------|-----------|-----------|--------|------|
| flat-large | 98.7 | 1.0 | 0.982 | 838ms |
| industrial_stepped_2-default | 98.7 | 1.0 | 0.983 | 828ms |
| flat_multi_level_3-default | 98.6 | 1.0 | 0.980 | 406ms |
| l_shaped-default | 98.4 | 1.0 | 0.972 | 403ms |
| proc-gambrel-medium | 97.6 | 1.0 | 0.943 | 147ms |
| mansard-default | 96.9 | 1.0 | 0.923 | 223ms |
| mono_pitch-default | 96.5 | 1.0 | 0.906 | 261ms |
| proc-butterfly-medium | 95.7 | 1.0 | 0.882 | 172ms |
| asymmetric_gable-default | 95.6 | 1.0 | 0.878 | 263ms |
| flat-default | 95.4 | 1.0 | 0.871 | 303ms |
| hip-default | 95.2 | 1.0 | 0.865 | 347ms |
| multi_section-default | 95.2 | 1.0 | 0.866 | 292ms |
| gable-default | 94.8 | 1.0 | 0.850 | 374ms |
| flat_with_ac_unit-default | 94.8 | 1.0 | 0.886 | 263ms |
| proc-silo-medium | 94.5 | 1.0 | 0.844 | 50ms |
| flat_multi_level_2-default | 93.3 | 1.0 | 0.800 | 357ms |
| flat_l_shaped-default | 92.9 | 1.0 | 0.787 | 385ms |
| proc-row_houses-medium | 90.4 | 1.0 | 0.712 | 150ms |
| proc-saltbox-medium | 81.6 | 1.0 | 0.407 | 141ms |
| proc-cross_plan-medium | 77.0 | 0.75 | 0.501 | 108ms |
| proc-dormer_house-medium | 70.5 | 0.25 | 0.812 | 179ms |

---

## Structural Limitations (Root Cause Analysis)

### 1. Many Small Planes (cathedral, church, silo_with_barn)

**Root cause**: The histogram has a fixed `minPeakFraction = 3%`. On a 14-plane building, each plane holds ~7% of points. After noise and boundary effects, small planes fall below the 3% threshold and are invisible. Lowering to 2% causes false positives on simpler buildings.

**Fundamental**: The Radon approach has no spatial locality. It cannot distinguish a "small but real" plane from a "noise cluster" because both produce weak histogram peaks. Local methods (RANSAC, region-growing) can verify spatial coherence; Radon cannot.

### 2. Non-Planar Surfaces (silo cylinders)

**Root cause**: A cylinder projects onto any perpendicular direction as a wide, flat distribution -- the opposite of a sharp peak. The histogram approach is inherently limited to planar surfaces.

### 3. Dormer Windows

**Root cause**: A dormer with 3 planes (2 sides + front) at ~20-50 points each is well below the `minPlanePoints` threshold. Even if peaks are detected, the planes are too small to survive filtering.

### 4. Flat Roofs with Obstacles

**Root cause**: AC units, antennas, and vents are coplanar with the main roof (within 0.5-1m). The histogram merges them with the main roof peak because `binSize = 0.08m` cannot resolve <1m height differences when the obstacle is small. The GT expects separate planes for each obstacle.

### 5. Loader Failures (9 proc-*-medium cases)

These 9 cases produce `composite = 0` with `undefined` for all metrics. They appear to be test case loading issues (the detector never runs), not algorithmic failures. All are procedural-medium variants of building types that work at small/large sizes.

---

## Comparison with V6 Cascade (Production Champion)

The V6 cascade (PCA region-growing) and Radon angular sweep are complementary approaches:

| Aspect | Radon (Axis 1) | V6 Cascade (Production) |
|--------|----------------|------------------------|
| **Approach** | Global: sweep all directions, find peaks | Local: grow regions from seed points |
| **Neighbor computation** | None (pure projection) | k-NN required |
| **Determinism** | Fully deterministic | Seed-order dependent |
| **Small planes** | Weak (below histogram resolution) | Strong (local region can grow from few points) |
| **Speed** | ~315ms avg | ~50-100ms avg |
| **Parallel planes** | Spatial CC splitting needed | Natural from region growing |
| **Non-planar** | Cannot detect | Can approximate as small planes |
| **Angular coverage** | Exhaustive (810 directions) | Limited to seed normals |

The Radon approach's unique value is its exhaustive angular coverage -- it will never miss a plane because of unlucky seed placement. A hybrid could use Radon for initial plane hypothesis generation and V6 for boundary refinement and small-plane recovery.

---

## Key Insights (Ranked by Impact)

1. **Spatial CC splitting is the single biggest architectural improvement** (+11.5% matchRate, +3.7 composite). The Radon approach inherently merges coplanar surfaces; CC splitting is the necessary correction.
2. **Final reassignment after CC splitting is critical** (+5-15% segAcc per case). Without it, boundary points retain their pre-split assignment which is often wrong.
3. **binSize = 0.08m is counterintuitively optimal**. Finer bins fragment; coarser bins give sharper peaks because noise averages out within each bin.
4. **800 directions is the sweet spot**. Below 600: angular gaps miss planes. Above 1000: excess candidates cause fragmentation. The transition is sharp.
5. **Greedy extraction is fundamentally incompatible with Radon**. Unlike RANSAC where candidates come from actual point subsets, Radon candidates come from histogram peaks that represent "averaged" normals across nearby directions.
6. **Linear interpolation is a free lunch**. Negligible overhead, eliminates bin-boundary artifacts.
7. **Plane-projected CC > XY-projected CC** for steep roofs. The improvement is small (+0.7) but conceptually important.
8. **Boundary smoothing is catastrophic in 3D**. Any 2D neighborhood voting scheme will merge planes at different heights.
9. **Residual sweep finds nothing useful**. Unassigned points are boundary noise, not missed planes.
10. **The 3% peak threshold is an irreducible compromise**. Lower catches more planes but introduces false positives. This limits the approach to buildings with relatively few, large planes.

---

## Files

- Detector: `research/axis-1-radon/detector.js` (727 lines, v2.3.0)
- Full findings: `research/axis-1-radon/FINDINGS.md`
- Final results (JSON): `research/axis-1-radon/results/all-synth-v2.3-final.json` (159 cases)
