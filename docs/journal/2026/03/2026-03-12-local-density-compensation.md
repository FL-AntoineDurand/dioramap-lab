---
id: "2026-03-12-local-density-compensation"
date: 2026-03-12
title: "Local Density Compensation: Density-Blind Algorithms Across the Pipeline"
tags: [density, cascade, segmentation, ridge-detect, point-weights, design]
category: design
status: concluded
refs: ["2026-03-12-spherical-kde-scan-line-compensation", "2026-03-12-spatial-linearity-k-expansion", "2026-03-12-tile-signals-unified-pca", "2026-03-12-density-aware-planarity-watershed"]
pipeline: v7
---

## Problem Statement

LiDAR point clouds have non-uniform spatial density. Three physical causes
dominate in IGN LiDAR HD data:

1. **Beam geometry**: scan lines are denser at nadir (directly below the
   aircraft) and sparser at swath edges. Within a single flight strip the
   density ratio can be 2:1 or more.
2. **Flight-strip overlap**: where adjacent strips overlap, point density
   roughly doubles. The boundary between single-coverage and double-coverage
   zones creates a sharp density discontinuity.
3. **Occlusion shadows**: vertical features (dormers, chimneys, parapets)
   cast LiDAR shadows — zones behind the obstruction receive no returns from
   that flight line. If the building is only in one strip, these shadows are
   absolute voids; if in two strips, they are half-density corridors.

Most of our segmentation pipeline treats every point as an equal vote.
A region with 200 pts/m^2 (strip overlap on a flat roof) contributes 4x more
influence than a region with 50 pts/m^2 (single-strip steep slope). This
biases every counting-based operation: histogram peaks, KDE amplitudes,
flood-fill connectivity, and minimum-size thresholds.

## Already Fixed: cascade-ridge-detect Method 3

**File**: `lib/pipeline/steps/cascade-ridge-detect.js`

Method 3 (k-NN boundary check) was the first algorithm where density bias
caused a visible failure. The original code used a fixed neighbor count:

```js
// OLD (v2): fixed K, no radius limit
const KNN_RIDGE_K = 8;
const neighbors = kNearest(kdTree, buildingPoints, buildingPoints[i], KNN_RIDGE_K);
```

In low-density zones (beam shadows, swath edges), the 8 nearest neighbors
could span 2-3 meters — reaching across a physical gap to a different roof
plane. The algorithm would then see neighbors on 2+ segments and flag the
point as a ridge candidate, producing **false ridge detections deep in the
interior of a plane** wherever density happened to be low.

The fix (v4, current HEAD) replaces fixed-K with radius search:

```js
// NEW (v4): radius-bounded, density-aware — cascade-ridge-detect.js:190-198
const ridgeRadius = params.ridgeKnnRadius ?? 0.5;
const minNeighbors = params.ridgeKnnMinNeighbors ?? 3;

const neighbors = radiusSearch(kdTree, buildingPoints, buildingPoints[i], ridgeRadius);
if (neighbors.length < minNeighbors) continue; // too sparse to judge
```

Key design decisions:
- **Radius = 0.5m**: physical neighborhood, independent of density.
- **minNeighbors = 3**: below this, the point is in a density void — skip
  rather than guess. This prevents false positives in beam shadows.
- Points that are skipped (too sparse) are neither flagged as ridge nor as
  clean — they are left for downstream steps (competitive-grow, point-stealing)
  to resolve.

### Test evidence

`test/ridge-knn-density.test.js` validates four density scenarios:

| Scenario | Geometry | Density pattern | Expected | Old (K=8) | New (radius) |
|----------|----------|-----------------|----------|-----------|--------------|
| A | Single plane | Gradient (0.1m to 0.5m spacing) | 0 ridges | false ridges in sparse tail | 0 ridges |
| B | Two-plane ridge | Uniform 0.15m | Ridges at junction only | correct | correct |
| C | Two-plane ridge | Sparse near junction | Ridges at junction, none in sparse interior | false ridges in sparse zone | correct |
| D | Single plane | Beam shadow gap (2m void) | 0 ridges | false ridges at gap edges | 0 ridges |

Scenario E explicitly demonstrates the regression: running with `radius=5.0,
minNeighbors=1` (approximating the old behavior) produces strictly more false
ridges than `radius=0.5, minNeighbors=3`.

## Density-Blind Algorithms Still Needing Fixes

### Stage 1 — Normal Orientation Grouping

**File**: `lib/pipeline/steps/cascade-shared.js`

#### Histogram variant (`histogramStage1`, line 137)

The 2D tilt x azimuth histogram increments a bin counter for each eligible
point (line 150):

```js
// cascade-shared.js:150
histogram[tb * azBins + ab]++;
```

Every point contributes +1 regardless of local density. A sparse roof plane
covering 20 m^2 at 30 pts/m^2 (600 points) produces a shorter peak than a
dense flat zone covering 5 m^2 at 200 pts/m^2 (1000 points). The sparse
plane may fall below `minPeakCount` and be missed entirely.

#### KDE variant (`kdeStage1`, line 264)

The KDE builds its sample array by pushing one value per point (line 277):

```js
// cascade-shared.js:277
eligTilts.push(tilts[i]);
```

The Gaussian KDE density estimate at any tilt value is proportional to the
number of sample points near that value. Dense zones inflate the KDE amplitude;
sparse zones are suppressed.

#### Pole singularity (both variants)

The tilt x azimuth parameterization has a pole at tilt = 0 degrees. Near-
horizontal normals have undefined azimuth (any rotation of a vertical vector
is still vertical). The histogram addresses this with a special flat-row
aggregation (line 153-165), but the KDE simply treats azimuth as circular
without adjusting for the shrinking metric at low tilt. Two flat planes at
different heights (same tilt ~ 0 degrees, arbitrary azimuth) are correctly
merged into one group, but two planes at tilt = 3 degrees with azimuths 170
degrees apart are incorrectly treated as a single peak because the KDE
bandwidth spans the full azimuth range at low tilt.

### Stage 2 — Offset Clustering

**File**: `lib/pipeline/steps/cascade-shared.js`, `runStage2And3` (line 534)

#### Histogram mode (line 736-738)

```js
// cascade-shared.js:736-738
for (let j = 0; j < offsets.length; j++) {
  const bin = Math.floor((offsets[j] - minOff) / offsetBinSize);
  offsetBins.set(bin, (offsetBins.get(bin) || 0) + 1);
}
```

Each point adds +1 to a bin. A roof plane that is half in a dense overlap
zone and half in a sparse single-strip zone will have its offset distribution
skewed toward the dense half's offset value. If the two halves differ
slightly in height (e.g., a slight warp), the dense side dominates the peak
location and the sparse side may not reach `minOffsetPeakCount`.

#### KDE mode (line 621-649)

Same issue: the offset KDE (`gaussianKDE(offsets, bandwidth, 200)`) treats
each sample point equally. The KDE amplitude is proportional to raw point
count, not to the physical area represented.

### Stage 3 — Spatial Clustering (clusterPointsXZ)

**File**: `lib/algorithms/_common.js`, line 64

```js
// _common.js:64-104
export function clusterPointsXZ(points, cellSize) {
  // ...
  const cols = Math.ceil((maxX - minX) / cellSize) + 1;
  // grid flood-fill: 4-connected adjacency
  for (const [dr, dc] of [[-1, 0], [1, 0], [0, -1], [0, 1]]) {
```

The grid uses fixed 1m cells (`SPATIAL_CELL_SIZE = 1.0` in cascade-shared.js,
line 37). A cell is either occupied (has at least one point) or empty. The
flood-fill treats all occupied cells as connected via 4-adjacency.

The failure mode: a continuous roof plane with a density void (beam shadow,
narrow occlusion corridor) creates an empty cell row/column. The flood-fill
cannot bridge it, and the plane is fractured into two spatial clusters. If
either fragment falls below `minSegmentPts`, it is discarded — losing valid
geometry.

This is particularly damaging for L-shaped or T-shaped planes where a single
scan-line gap along the narrow section bisects the plane.

### Minimum Segment Threshold

**File**: `lib/pipeline/steps/cascade-shared.js`, line 553

```js
// cascade-shared.js:553
const minSegmentPts = Math.max(3, Math.round(baseMinSeg * scale));
```

`DEFAULT_MIN_SEGMENT_PTS = 10` (line 45). A large sparse plane (e.g., a 15 m^2
dormer slope at 8 pts/m^2 in an occlusion shadow = 120 points) easily passes.
But a smaller sparse plane (e.g., a 3 m^2 chimney shelter at 8 pts/m^2 = 24
points) is at risk if spatial clustering fragments it. In contrast, a tiny
dense feature (1 m^2 at 200 pts/m^2 = 200 points) always passes despite
representing 1/3 the physical area. The threshold conflates point count with
geometric significance.

## Proposed Solution: Point-Weights Artifact

### Concept

Compute a per-point weight `w[i]` = the area that point `i` represents.
Points in sparse zones get high weights; points in dense zones get low
weights. All downstream operations accumulate `w[i]` instead of `+1`.

### Computation

Compute in `tile-signals` step (which already builds the kd-tree and runs
kNN for every point). Local density from kNN:

```
density_i = k / (pi * d_k^2)
```

where `d_k` is the distance to the k-th nearest neighbor (the boundary of
the ball containing exactly k points). This is the standard kNN density
estimator.

The weight is the inverse density, proportional to the Voronoi cell area
that the point represents:

```
w_i = 1 / density_i = pi * d_k^2 / k
```

Pseudocode for `tile-signals.js` (after the existing kNN loop at line 250):

```js
// New: compute per-point spatial weights
const pointWeights = new Float32Array(totalPts);
const WEIGHT_K = 8;  // use k=8 for density estimate (local enough)

for (let i = 0; i < totalPts; i++) {
  const neighbors = kNearest(kdTree, points, points[i], WEIGHT_K);
  if (neighbors.length < WEIGHT_K) {
    pointWeights[i] = 1.0;  // edge of cloud, default weight
    continue;
  }
  // d_k = distance to k-th neighbor
  const dk = Math.sqrt(neighbors[WEIGHT_K - 1].distSq);
  // weight = pi * dk^2 / k (area per point)
  pointWeights[i] = Math.PI * dk * dk / WEIGHT_K;
}

// Normalize so that mean weight = 1 (preserves absolute counts downstream)
const wSum = pointWeights.reduce((s, v) => s + v, 0);
const wScale = totalPts / wSum;
for (let i = 0; i < totalPts; i++) pointWeights[i] *= wScale;

ctx.addArtifact('point-weights', 'array', pointWeights, {
  sourceStep: 'tile-signals',
  colorMode: 'gradient',
  description: 'Per-point area weight (mean=1). Sparse → high, dense → low.'
});
```

Note: `tile-signals` already fetches K=32 neighbors for every point (line
252). The k=8 slice is a subset of those — no additional kd-tree queries
needed if we extract `d_8` from the existing neighbor list.

### Downstream Integration

#### Stage 1 — weighted histogram

```js
// Replace: histogram[tb * azBins + ab]++;
// With:    histogram[tb * azBins + ab] += weights[i];
```

Bins now accumulate area, not count. The `minPeakCount` threshold becomes
a minimum *effective point count* (sum of weights), preserving the existing
semantic.

#### Stage 1 — weighted KDE

```js
// Replace: eligTilts.push(tilts[i]);
// With:    eligTilts.push(tilts[i]); eligWeights.push(weights[i]);
// Then pass weights to a weighted KDE variant:
//   KDE(x) = (1/Σw) * Σ w_i * K((x - x_i) / h)
```

#### Stage 2 — weighted offset histogram / KDE

Same pattern: `offsetBins[bin] += weights[i]` instead of `+= 1`.

#### Stage 3 — density-aware spatial clustering

Two options:
1. **Adaptive cell size**: set `cellSize = f(local_density)` — complex,
   breaks the regular grid assumption.
2. **Bridge tolerance**: allow flood-fill to jump over 1-2 empty cells if
   adjacent occupied cells have low weight (= sparse zone). Simpler to
   implement: when processing a cell's neighbors, also check 2-hop neighbors
   if the 1-hop cell is empty but the query cell's mean weight exceeds a
   bridge threshold.

Option 2 pseudocode:

```js
// In clusterPointsXZ, when neighbor cell is empty:
if (!grid.has(neighborCell) && bridgingEnabled) {
  // Check 2-hop: is there an occupied cell beyond the gap?
  const twoHop = neighborCell + dr * cols + dc;
  if (grid.has(twoHop) && meanWeight(currentCell) > BRIDGE_THRESHOLD) {
    queue.push(twoHop);  // bridge the gap
  }
}
```

#### Min segment threshold

Replace `points.length >= minSegmentPts` with
`sumWeights(segment) >= minSegmentEffective`. Since mean weight = 1, the
threshold value stays the same numerically but now measures effective area
coverage rather than raw count.

## Proposed Solution: Hemisphere Normal Accumulation

### Problem with Separable Tilt x Azimuth

The current normal grouping (both histogram and KDE variants) uses a
separable parameterization:

1. Find tilt peaks (1D KDE on tilt values)
2. For each tilt peak, find azimuth peaks (1D circular KDE)

This has three problems:

- **Pole singularity**: azimuth is undefined at tilt = 0 degrees. The code
  special-cases flat normals, but the transition zone (tilt 2-8 degrees)
  has poorly defined azimuth that degrades both KDE and assignment.
- **Coupling loss**: a separable product of 1D KDEs cannot represent
  correlated tilt-azimuth distributions. A ridge between two 30-degree
  slopes at azimuths 0 and 180 degrees creates a single broad tilt peak
  with two sharp azimuth sub-peaks — the separable approach handles this.
  But a hip roof with slopes at tilt 25 and 35 degrees at azimuths 0, 90,
  180, 270 degrees creates a tilt KDE with a single merged peak around
  30 degrees, losing the tilt variation.
- **Non-equal-area bins**: in the histogram variant, a tilt bin near 0
  degrees covers much less solid angle than a bin near 90 degrees, biasing
  peak detection toward steep orientations.

### HEALPix Equal-Area Spherical Binning

Replace the separable tilt x azimuth parameterization with HEALPix
(Hierarchical Equal Area isoLatitude Pixelisation) on the upper hemisphere.

HEALPix at resolution N_side divides the sphere into 12 * N_side^2 pixels
of equal area. On the upper hemisphere: 6 * N_side^2 pixels. At N_side = 8:
384 pixels, each spanning approximately 10.4 degrees. At N_side = 16: 1536
pixels, each approximately 5.2 degrees.

Pseudocode for weighted hemisphere accumulation:

```js
const NSIDE = 8;
const NPIX_HEMI = 6 * NSIDE * NSIDE;  // 384

// Accumulate weighted normals into HEALPix bins
const hpxBins = new Float64Array(NPIX_HEMI);

for (let i = 0; i < totalPts; i++) {
  if (!eligible[i]) continue;
  const n = normals[i];
  // Ensure upper hemisphere (flip if pointing down)
  const ny = n.y < 0 ? -n.y : n.y;
  const nx = n.y < 0 ? -n.x : n.x;
  const nz = n.y < 0 ? -n.z : n.z;
  // Convert to spherical: theta = acos(ny), phi = atan2(nz, nx)
  const theta = Math.acos(Math.min(1, Math.max(-1, ny)));  // 0 = up
  const phi = Math.atan2(nz, nx);
  const pixel = healpy_ang2pix(NSIDE, theta, phi);  // HEALPix pixel index
  hpxBins[pixel] += weights[i];  // weighted accumulation
}
```

### Peak Finding on the Sphere

Find peaks by scanning HEALPix pixels for local maxima among their
(typically 8) neighbors:

```js
const peaks = [];
for (let pix = 0; pix < NPIX_HEMI; pix++) {
  if (hpxBins[pix] < minPeakWeight) continue;
  const neighbors = healpy_get_neighbors(NSIDE, pix);
  let isMax = true;
  for (const nb of neighbors) {
    if (nb >= 0 && nb < NPIX_HEMI && hpxBins[nb] > hpxBins[pix]) {
      isMax = false;
      break;
    }
  }
  if (isMax) {
    const [theta, phi] = healpy_pix2ang(NSIDE, pix);
    peaks.push({ pixel: pix, theta, phi, weight: hpxBins[pix] });
  }
}
```

### Point-to-Peak Assignment via Great-Circle Distance

Replace tilt/azimuth distance with great-circle (angular) distance between
the point's normal and each peak's mean normal:

```
angle = acos(dot(n_point, n_peak))
```

This is singularity-free: works at the pole (tilt=0), at the equator
(tilt=90), and everywhere in between. Assignment uses a single angular
threshold (e.g., 12 degrees) instead of separate tilt and azimuth tolerances.

### Advantages

| Aspect | Separable tilt x azimuth | HEALPix hemisphere |
|--------|--------------------------|---------------------|
| Pole singularity | Yes (azimuth undefined at tilt=0) | No (equal-area everywhere) |
| Coupling | No (separable product) | Full 2D distribution |
| Area bias | Yes (tilt bins have unequal solid angle) | No (HEALPix pixels are equal-area) |
| Distance metric | Manhattan in (tilt, azimuth) space | Great-circle angle (true angular distance) |
| Density compensation | Needs weighted KDE | Naturally via `+= weight[i]` |
| Visualization | 2D heatmap (rectangular, distorted) | Mollweide/orthographic projection |

## Proposed New Step: hemisphere-normals-density-aware-segment

This step would replace the three stages of `cascade-shared.js` (Stage 1 +
Stage 2 + Stage 3) with density-aware equivalents:

1. **Hemisphere normal grouping**: HEALPix accumulation with point-weights,
   peak finding on the sphere, great-circle assignment.
2. **Weighted offset separation**: per-group offset KDE with weighted samples.
3. **Bridging spatial clustering**: grid flood-fill with 1-cell bridging for
   sparse zones (or kNN-graph connectivity as alternative).

The step would consume:
- `building-points`, `building-count`, `point-normals` (existing)
- `point-weights` (new artifact from tile-signals)
- `kd-tree` (existing)

And produce the same output artifacts as the current cascade:
- `cascade-segments`, `cascade-segment-list`, `cascade-segment-stats`
- `ridge-candidates`
- `cascade-normal-histogram` (now a HEALPix heatmap)
- `cascade-offset-histograms`

This ensures full backward compatibility with all downstream steps
(cascade-ridge-detect, competitive-grow, point-stealing, plane-fit, etc.).

## Implementation Priority

| Fix | Impact | Complexity | Priority |
|-----|--------|------------|----------|
| `point-weights` artifact in tile-signals | Foundation for all other fixes | Low (data already available) | **1 — do first** |
| Weighted Stage 2 offset histogram/KDE | Moderate (fixes double-strip bias) | Low (pass weights array) | **2** |
| Weighted Stage 1 histogram | Moderate (fixes missed sparse planes) | Low | **3** |
| Bridging spatial clustering | High (fixes fractured segments) | Medium | **4** |
| Weighted min-segment threshold | Low (rare edge case) | Trivial | **5** |
| HEALPix Stage 1 replacement | High (fixes pole + coupling) | High (new math, new viz) | **6 — research** |

Steps 1-5 are incremental improvements that can be validated independently.
Step 6 is a larger redesign that requires a HEALPix implementation (either
a JS port of the `ang2pix`/`pix2ang`/`get_neighbors` functions, or a
lightweight npm package).

## Implementation (v7 pipeline)

The design above was implemented as the `v7` pipeline with these key
deviations from the original proposal:

### Lambert instead of HEALPix

Instead of HEALPix, we use **Lambert Azimuthal Equal-Area projection** onto
a 128×128 grid (`lib/geometry/lambert.js`). This is simpler (no HEALPix
library needed), provides sub-pixel interpolation via bilinear splatting,
and naturally handles the pole: flat roofs (ny≈1) map to the disc center
with no singularity.

### Tile-wide architecture

The `v7` pipeline runs segmentation on the **full tile point cloud** (all
buildings), not scoped to a single building via extract-denoise. The step
chain is:

```
tile-signals → planarity-watershed → hemisphere-normals-density-aware-segment
  → normal-cascade-seed → competitive-grow → … → hull-extraction → lod2
```

`tile-signals` produces `building-points`, `point-normals`, `kd-tree`,
`point-weights`, and `denoised-to-orig` for the entire tile. Hull-extraction
clips to per-building footprints afterward.

### No cascade-ridge-detect

Ridge detection is omitted from v7. All downstream steps (normal-cascade-seed,
competitive-grow, point-stealing, ridge-edge-fit) now handle missing
`ridge-candidates` gracefully, defaulting to an empty mask.

### Results

Initial benchmark: 7/7 pass, avg score 83.4, ~25s/case. The tile-wide
approach processes more points than building-scoped (full tile vs single
building), hence higher latency. Score delta vs v6g (90.9) reflects the
different segmentation strategy — further tuning expected.
