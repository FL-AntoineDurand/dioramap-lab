---
id: "2026-03-15-v7-compare-segmentation-arena"
date: 2026-03-15
title: "V7-Compare: Segmentation Arena — 14 parallel approaches, density-improved, scored in-pipeline"
tags: [v7, architecture, segmentation, comparison, density, arena, branch-score, parallel-agents, master-plan]
category: design
status: proposed
refs:
  - "2026-03-13-v7-master-plan"
  - "2026-03-07-ten-axis-research-session-report"
  - "2026-03-06-four-axis-research-synthesis"
  - "2026-03-15-ransac-full-density-compensation"
  - "2026-03-15-ransac-poly-curved-surface-detection"
  - "2026-03-13-cascade-ridge-detect-methods-1-2-retirement"
  - "2026-03-13-night-architecture-refactor"
  - "2026-03-12-local-density-compensation"
  - "2026-03-06-axis1-radon-plane-detection"
  - "2026-03-06-axis2-multiview-rasterize-detect"
  - "2026-03-06-axis3-spectral-frequency-methods"
  - "2026-03-06-axis4-curvature-pca-region-growing"
  - "2026-03-12-density-aware-planarity-watershed"
  - "2026-03-06-meta-analysis-pipelines-vs-axes"
  - "2026-03-06-next-gen-pipeline-research-axes"
pipeline: v7-compare
---

# V7-Compare: Segmentation Arena

## 1. Why This Exists

### 1.1. The history

The project has explored segmentation from many angles:

**Pipeline generations (V2–V7)**:
- **V2** (Feb 2026): brute-force RANSAC (3 random points → plane → grow → steal). Score: 98.9 on quick set. No normals, purely geometric. Still the single-building-scope champion.
- **V3**: hybrid flat-detect + histogram seed for horizontal planes + RANSAC for slopes.
- **V4**: normal histogram seeding (tilt×azimuth fixed bins). First to use normals.
- **V5**: multimodal (DSM + ortho + normals). Experimental.
- **V6** (7 histogram variants + 8 KDE variants = 15 pipelines): three-stage normal cascade — orientation groups → offset separation → spatial clustering. KDE refinement in stages 1 and/or 2. Best: v6g-kde-full at 90.9 avg on quick set.
- **V7** (Mar 2026): density-aware hemisphere segmentation — Lambert equal-area projection + weighted offset KDE + radius-graph CC. Tile-wide scope. Score: 83.4 avg (lower because tile-wide + no footprint association yet).

**4-axis research session (Mar 6, 2026)**:
Four autonomous agents explored fundamentally different mathematical traditions in parallel over ~10 hours, using 159 synthetic test cases + 25 quick cases with a **research-specific scorer** (different from bench.db's composite_v2).

| # | Approach | Math tradition | Quick | All-Synth (159) | Versions |
|---|----------|---------------|-------|-----------------|----------|
| 4 | PCA Curvature + Region Growing | Differential geometry | 98.2 | **96.4** | 42 |
| 2 | Multi-View Rasterize-Detect | Computer vision | 97.2 | 95.1 | 10+ |
| 3 | Spectral/Agglomerative | Harmonic analysis | 95.1 | 91.3 | 15 |
| 1 | CT/Radon Angular Sweep | Integral geometry (tomography) | 93.0 | 88.7 | 15+ |

**Critical finding — universal convergence**: Despite starting from completely different mathematical traditions, all 4 axes independently converged on the same 3-phase decomposition:

```
Phase 1: Normal/Orientation Grouping
  Axis 1: 810 Fibonacci hemisphere projections → distance histograms
  Axis 2: 20×20 normal histogram → greedy peak merging
  Axis 3: Agglomerative clustering on unit sphere
  Axis 4: PCA eigenvalue classification (planarity/linearity/sphericity)

Phase 2: Height/Offset Separation
  Axis 1: Distance histogram per direction → peak detection
  Axis 2: 1D offset histogram (0.08m bins) → valley detection
  Axis 3: 1D offset histogram (0.3m bins) → valley detection
  Axis 4: Adaptive distance threshold during region growth

Phase 3: Spatial Connected Components
  Axis 1: Grid flood fill (0.45m cells)
  Axis 2: 2D raster CC (adaptive cell = max(0.1m, 1.5×avgSpacing))
  Axis 3: Grid CC (radius = 2× median kNN distance)
  Axis 4: k=20 kNN flood fill + 16-stage post-processing
```

This convergence across independent agents is evidence that the 3-phase decomposition (orient → offset → spatial) is **fundamentally correct** for roof plane detection.

**Scorer bias**: The 4-axis research scorer evaluated planar segmentation only. Ground truth was flat planes. Zero reward for detecting curved surfaces — a cylinder detected as a cylinder scores 0; the same cylinder approximated as 12 faceted planes matching GT scores full marks. This biased all agents toward planar refinement and away from curvature detection.

**Key techniques discovered in the 4-axis session** (some absent from 10-axis):
- **Iterative cascade refinement** (Axis 3): 3 rounds of assign→refit. The single most impactful technique — segAcc jumps from 0.14-0.53 to 0.92-1.0
- **Steal-back boundary refinement** (Axis 4): reverse pass swapping border points to better-fitting neighbor plane (+0.2 avg, +1.7 on gambrel)
- **Collinear normal inheritance** (Axis 4, v0.28): eigenvalue ratio l2/l1 < 0.05 → parapet. +15.1 composite on parapet cases
- **Offset histogram bin size 0.08m** (Axis 2): sweet spot between noise and meaningful separation. Simpler than KDE, works better.
- **Anti-normal ray convergence** (Axis 4 stages 13-17): converging normals → curved surface. Prototype only; "doesn't affect score since silo GT is plane-based"
- **Silo protection** (Axis 4): curvature-variation-aware absorption tightening prevents over-merging curved fragments
- **Spatial re-split with RMS guard** (Axis 4, v0.33): grid-based CC protected by residual quality metric. +12.2 on affected cases.
- **Adaptive cell-size raster CC** (Axis 2): `max(0.1m, 1.5×avgSpacing)` — self-calibrating to data density

**Meta-analysis conclusion** (`2026-03-06-meta-analysis-pipelines-vs-axes.md`): Three philosophical traditions exist — (A) top-down global (Radon: find global hypotheses, carve into pieces), (B) decompose-then-assemble (cascade: orient → offset → spatial), (C) bottom-up local (region growing: start from points, grow outward). An ensemble combining these complementary approaches could beat any individual.

**10-axis research session (Mar 6-7, 2026)**:
Ten independent subagents each explored a radically different algorithm family on 30 test cases (quick set), scored with V3 scorer (23 metrics). Results (authoritative, from `2026-03-07-ten-axis-research-session-report.md`):

| # | Approach | Overall | Planar | Plane Match | Curved | Curved Match | Seg. Accuracy |
|---|----------|---------|--------|-------------|--------|-------------|---------------|
| 5 | Strip Consensus | **93.0** | **97.4** | **98.9%** | **88.7** | 83.3% | — |
| 9 | Edge-First | 92.8* | 94.8 | 92.5% | 90.7 | **100.0%** | **85.9%** |
| 8 | Noise RANSAC | 88.6 | 94.4 | 91.1% | 82.8 | 80.0% | 81.3% |
| 10 | Density Topology | 86.9 | 90.8 | ~89% | ~83 | 83.3% | ~74% |
| 6 | Curvature-First | 86.4 | — | 87.5% | — | **93.3%** | 68.4% |
| 1 | Radon Sweep | 84.5 | 93.4 | 88.5% | 70.4 | 41.7% | 82.6% |
| 2 | Multiview Raster | 80.1 | 83.4 | 84.9% | 71.9 | 83.3% | 64.4% |
| 7 | MultiReturn Edge | 79.4 | 82.1 | 92.8% | 76.7 | 80.0% | 76.6% |
| 4 | PCA Region Growing | 79.3 | 79.0 | 88.3% | 79.6 | 73.3% | 57.7% |
| 3 | Spectral Graph | 66.9 | 75.8 | 86.1% | 58.0 | 56.7% | — |

*Axis 9 includes 15-point thesis bonus; 76.9 without.

**4-axis earlier session** (Mar 6): Axes 1-4 were also scored on different test sets with a different scorer, yielding different numbers (Axis 4: 98.2, Axis 2: 97.2). These numbers are NOT directly comparable with the 10-axis table above. The 10-axis table is the authoritative cross-comparison.

**Post-V7 work** (Mar 13-15):
- `ransac-full` density-compensated: pool give-back, pair-radius CC, grow pass (dead-end in V7)
- `ransac-poly` v4: polynomial RANSAC for curved surfaces — dome/cylinder/cone/saddle detection (dead-end in V7)
- `planarity-watershed` v2: density-aware watershed on planarity field (Axis 10, dead-end in V7)
- Night architecture refactor: declarative wiring, global index, artifact schemas — makes new pipeline wiring trivial

### 1.2. The problem

All these approaches exist in isolation:
- Research detectors are standalone functions in `research/*/detector.js` — they cannot be scored in the pipeline benchmark, cannot produce intermediate visualizable artifacts, and don't benefit from V7 improvements (density compensation, point-weights, tile-signals preprocessing).
- V6 cascade variants are building-scoped pipelines that run through the full post-processing chain — comparing their segmentation quality requires running separate benchmarks.
- The 3 dead-ends in V7 (ransac-full, ransac-poly, planarity-watershed) produce `owner` arrays but are not scored — there's no way to see their composite score alongside the main pipeline's score.

### 1.3. The aim

Build a **single comparison pipeline** (`v7-compare`) that runs **14 parallel segmentation chains** as dead-end branches, each:
1. **Implemented as proper pipeline steps** (not wrappers around research detectors) with visualizable intermediate artifacts
2. **Density-improved** with all V7 discoveries: point-weights, area-based thresholds, radius-based CC, anti-pattern guards
3. **Scored in-pipeline** via a new `branch-score` step that calls the full metrics suite and emits a score artifact
4. **Visible in the UI** as parallel branches with score badges on each terminal node

This creates a **segmentation arena** where every approach competes on the same test cases, with the same post-processing (grow + steal), the same density compensation, and the same scorer — apples-to-apples.

### 1.4. The convergence problem and genuine root ideas

All 10 research axes converged to the same 3-phase decomposition (orient → offset → spatial CC). This convergence is mathematically inevitable — planes are 5-parameter objects (3D normal + 1D offset + 2D spatial footprint) and this decomposition minimizes parameter space at each phase. But it means the "final implementations" are all variations of the same approach with different tuning.

The **genuine root ideas** — the mathematical theses BEFORE convergence — are more interesting. Several axes started from truly different places but were pulled toward the standard decomposition by the scorer bias (rewarding planar accuracy only) and the practical need to beat 90+ scores. Here's what each axis COULD have been:

#### Genuinely different approaches (stayed closest to root thesis)

**Axis 5 — Strip-aware consensus** (99% thesis authentic): The only axis that genuinely used its proposed methodology end-to-end. Each flight strip is an independent observation; consensus across strips confirms real surfaces. This is a **multi-observer fusion** problem, not a clustering problem. A pure strip-aware chain should process strips independently and merge via voting, not fall back to global normal clustering.

**Axis 9 — Edge-first boundary detection** (100% thesis bonus): Inverts the standard decomposition. Instead of finding surfaces and inferring boundaries, detect boundaries first via normal gradient, then flood-fill between them. The edge-map IS the primary output. This is a fundamentally different information flow — boundary → region instead of region → boundary.

**Axis 4/6 — Curvature-first classification** (viable at 94-97 estimated): Classify each point's surface type (planar/cylindrical/spherical/conical) from the local curvature tensor BEFORE fitting. Grow only same-type regions. This naturally handles curved surfaces without post-hoc recovery — the type is known upfront. The research scorer's planar-only GT killed incentive to pursue this, but for our arena (where we care about curved detection) it's the most promising root idea.

#### Root ideas that were abandoned (could be revisited)

**Axis 2 original — Per-strip rasterization + Hough line detection**: The original vision was to rasterize each flight strip as a 2D image, run Hough line detection for ridge/eave edges, and back-project to 3D. This was abandoned because the test harness didn't pass pointSourceId metadata. Now that metadata IS available, the pure multi-view approach could work: per-strip height rasters → Sobel edge detection → Hough lines → 3D ridge candidates → grow regions between ridges. This is closer to computer vision than point cloud processing.

**Axis 1 original — Pure Radon transform**: The idea was borrowed from CT tomography — use 810 projections to detect planes the same way CT uses projections to reconstruct volumes. The pure approach failed because histogram peaks can't distinguish coplanar disconnected surfaces. But the Radon direction-scoring heatmap (which directions produce sharp peaks?) is a unique artifact that no other approach produces. A hybrid could use Radon scores as priors for a downstream segmenter.

**Axis 3 original — Graph Laplacian spectral segmentation**: Failed completely on roof geometry (Fiedler cut goes along building long axis, not ridges). BUT: the failure is specifically for spatial-only graphs. A graph where edge weights encode **normal similarity** (binary gating: cut edges where normal angle > threshold) could work — this was Axis 3's pivot discovery (v0.5). A "spectral on normal-gated graph" approach hasn't been fully explored.

**Axis 10 — Watershed on scalar fields**: Using planarity as a topographic field and watershed segmentation to find basins. This is topology-based — seeds at planarity maxima, grow by steepest descent. The approach is sound but planarity is noisy at boundaries (exactly where precision matters). An alternative: watershed on a **smoothed** planarity field, or on a different scalar (e.g., normal coherence, eigenvalue gap).

#### What this means for the arena

The arena should include BOTH:
- **Converged implementations** (the practical ones that score well) — these are the A1-A9 chains described in §5
- **Pure-thesis experiments** where feasible — chains that stay true to the original mathematical idea even if they score lower. The point is to evaluate whether the root idea has merit when combined with V7 density compensation, not to maximize score.

For the arena, the implementing agent should **read the original thesis** in the research journal (§8.6 references) and decide whether the converged implementation or a pure-thesis version is more informative. Both are valid dead-end branches.

**Five pure-thesis chains (T1-T5) are specified in §5.1-5.5.** Full algorithmic pseudocode in `2026-03-15-five-pure-thesis-chains.md`. These are raw algorithms that agents must refine, test, fix and improve — WITHOUT leaving the thesis:

| Chain | Thesis | Core idea |
|-------|--------|-----------|
| **T1** | Integral Geometry (Radon) | Project along 819 directions → 1D histograms → peaks ARE planes. Ramp-filtered back-projection. No spatial reasoning. |
| **T2** | Computer Vision (Height-Image) | Rasterize to DSM → Sobel edge detection → Hough line detection → region growing between edges. The building is an IMAGE. |
| **T3** | Spectral Geometry (Graph Laplacian) | Normal-gated k-NN graph → Laplacian eigendecomposition → eigencuts at ridges. Eigenvalue gap → automatic K. No plane model. |
| **T4** | Differential Geometry (Curvature-First) | Exact principal curvatures → type classification (plane/cylinder/sphere/cone/saddle) → type-homogeneous region growing → type-specific fitting. |
| **T5** | Frequency Domain (DCT/JPEG) | Rasterize to DSM → block DCT → quantize like JPEG → what survives IS the core structure. Difference = structural edges. Multi-scale decomposition. |

---

## 2. Architecture

### 2.1. Pipeline structure

```
                                    tile-signals (shared once)
                                           │
  ┌──────┬──────┬──────┬──────┬──────┬─────┼─────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
  ▼      ▼      ▼      ▼      ▼      ▼     ▼     ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼
 T1     T2     T3     T4     T5     A5    A6     A7     A8     A9    RFull  RPoly  PWat  Hemi
Radon  Height Spectr Curv   DCT/   Strip CurvF  MRet   Noise  Edge  RANSAC Poly   Wshd  Main
tomo   image  eigen  type   JPEG   cons  class  edge   RANSAC first  full   RANSAC       V7
PURE   PURE   PURE   PURE   PURE
  │      │      │      │      │      │      │      │      │      │      │      │      │
  ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼
 CG     CG     CG     CG     CG     CG     CG     CG     CG    (own   (own   (own  NCS
  │      │      │      │      │      │      │      │      │    grow)  grow)  grow)   │
 PS     PS     PS     PS     PS     PS     PS     PS     PS      │      │      │    CG
  │      │      │      │      │      │      │      │      │      │      │      │      │
 PfO    PfO    PfO    PfO    PfO    PfO    PfO    PfO    PfO    PfO    PfO    PfO   PS
  │      │      │      │      │      │      │      │      │      │      │      │      │
 BS     BS     BS     BS     BS     BS     BS     BS     BS     BS     BS     BS    PfO
[scr]  [scr]  [scr]  [scr]  [scr]  [scr]  [scr]  [scr]  [scr] [scr]  [scr]  [scr]   │
                                                                                     BS
                                                                                    [scr]

CG = competitive-grow, PS = point-stealing, PfO = planes-from-owner, BS = branch-score
NCS = normal-cascade-seed (main V7 only)
```

Each dead-end chain: `[seeding step(s)] → competitive-grow → point-stealing → planes-from-owner → branch-score`

Three existing dead-ends (ransac-full, ransac-poly, planarity-watershed) already produce `owner` + `seed-planes` + `plane-equations` internally. They get `planes-from-owner → branch-score` appended.

The hemisphere main path keeps its existing chain: `hemisphere → normal-cascade-seed → competitive-grow → point-stealing → planes-from-owner → branch-score`.

**No post-processing chain** (hull, wall-roof, edges, lod2) in v7-compare. It's segmentation-only. The full post-processing lives in `v7` pipeline.

### 2.2. Shared artifacts from tile-signals

Every chain reads from the same `tile-signals#0` output:

| Artifact | Type | Used by | Notes |
|----------|------|---------|-------|
| `positions` | Float32Array(N×3) | All | Raw point coordinates |
| `building-indices` | Uint32Array | All | Indices of tile points to segment |
| `point-normals` | Float32Array(N×3) | A1-A9, hemi, pwat | PCA normals at K=32 |
| `point-weights` | Float32Array(N) | All | Voronoi area per point (density compensation) |
| `kd-tree` | transient | All | Shared spatial index |
| `planarity` | Float32Array(N) | A4, A6, pwat | PCA planarity ratio |
| `curvatures-k16` | Float32Array(N×2) | A4, A6 | Principal curvatures |
| `linearity` | Float32Array(N) | A4, A6 | PCA linearity ratio |
| `scattering` | Float32Array(N) | A4, A6 | PCA scattering ratio |
| `eigen-reliability` | Float32Array(N) | A4 | Eigenvalue spread quality |
| `lidar-density` | number | All | Global density estimate |
| `lidar-z-noise` | number | All | LiDAR measurement noise σ_z |
| `classifications` | Uint8Array(N) | PfO, BS | LAS classification codes |

LiDAR metadata artifacts (injected by `runner.js`, NOT by tile-signals):

| Artifact | Type | Used by | Notes |
|----------|------|---------|-------|
| `scan-angles` | Int16Array(N) | A5, A8 | Scan angle rank (×100 for PDRF 6) |
| `point-source-ids` | Uint16Array(N) | A5 | Flight strip ID per point |
| `return-numbers` | Uint8Array(N) | A7 | Return number (1-based) |
| `number-of-returns` | Uint8Array(N) | A7 | Total returns for this pulse |

These metadata artifacts are already present in `runner.js` (lines 109-112, 383-386) and in the binary test case format (`test-case-binary.js`). Synthetic test cases may lack metadata — steps must handle null gracefully.

### 2.3. Shared post-seeding chain

After each seeding step produces `owner` (Int8Array, globally indexed) + `seed-planes` (Array) + `plane-equations` (Array), the shared chain runs:

1. **`competitive-grow`** (existing, v9) — density-compensated min-heap growth. Reads `owner`, `seed-planes`, `plane-equations`, `point-weights`, `kd-tree`. Writes updated `owner`.
2. **`point-stealing`** (existing, v9) — density-compensated boundary refinement. Reads `owner`, `plane-equations`, `point-weights`, `kd-tree`. Writes updated `owner`.
3. **`planes-from-owner`** (existing, v2) — converts `owner` groups to plane result objects with hull, area, tilt. Reads `owner`, `seed-planes`, `plane-equations`, `point-weights`, `classifications`. Writes `planes` array (standard format for scorer).
4. **`branch-score`** (NEW) — calls `computeAllMetrics()` on the branch's `planes`. Writes `branch-score` metadata artifact.

Each chain gets its own instance numbers (`competitive-grow#0`, `#1`, ..., `#12`). The declarative wiring resolver (from R1 refactor) auto-routes artifacts between instances.

### 2.4. Step output contract

Every new seeding step MUST produce these artifacts:

```javascript
artifacts: {
  writes: [
    { name: 'owner', type: 'array', description: 'Int8Array(N): point → plane index, -1 = unassigned. Globally indexed.' },
    { name: 'seed-planes', type: 'metadata', description: 'Array of seed plane objects with inlierIndices, centroid, area.' },
    { name: 'plane-equations', type: 'metadata', description: 'Array of {normal: {x,y,z}, d: number} plane equations.' },
  ],
}
```

This is the same contract as `hemisphere-normals-density-aware-segment`, `ransac-full`, `normal-cascade-seed`, etc. The shared chain downstream depends on these exact artifact names.

Additionally, seeding steps SHOULD produce intermediate visualization artifacts specific to their approach (see per-step sections below). These are dead-end artifacts that don't feed downstream steps but are visible in the UI.

---

## 3. Foundation: `branch-score` step

### 3.1. Purpose

Score any segmentation chain's output in-pipeline, producing a metrics artifact visible in the UI and accessible to benchmark.js.

### 3.2. Interface

```javascript
export const meta = {
  name: 'branch-score',
  version: 1,
  description: 'Scores a segmentation branch by calling computeAllMetrics on its planes artifact. Produces a branch-score metadata artifact with all metrics.',
  params: [],
  artifacts: {
    reads: [
      'planes',              // from planes-from-owner (the branch's output)
      'positions',           // from tile-signals (shared)
      'classifications',     // from tile-signals (shared)
      'input-gt',            // ground truth: { planes, surfaces, edges, pointPlaneIds }
      'input-bdtopo',        // footprint polygon
    ],
    writes: [
      { name: 'branch-score', type: 'metadata', description: 'Full metrics object: composite, segF1, coverage, residual, detection rate, etc.' },
    ],
  },
};
```

### 3.3. Implementation sketch

```javascript
import { computeAllMetrics } from '../../metrics/index.js';

export function fn(ctx, params) {
  const planes = ctx.getArtifact('planes').data;
  const positions = ctx.getArtifact('positions').data;
  const classifications = ctx.getArtifact('classifications').data;

  // Ground truth — may be absent (real test cases without GT)
  const gtArt = ctx.hasArtifact('input-gt') ? ctx.getArtifact('input-gt').data : null;
  const footprintArt = ctx.hasArtifact('input-bdtopo') ? ctx.getArtifact('input-bdtopo').data : null;

  if (!gtArt || !footprintArt) {
    // No GT available — emit empty score
    ctx.addArtifact('branch-score', 'metadata', { composite: null, reason: 'no-gt' });
    return;
  }

  const groundTruth = gtArt.groundTruth || gtArt;
  const footprint = footprintArt.footprint || footprintArt;
  const pointPlaneIds = gtArt.pointPlaneIds || [];
  const footprintOffset = gtArt.footprintOffset || null;

  const metrics = computeAllMetrics(
    { planes, surfaces: [], edges: [] },
    groundTruth,
    footprint,
    positions,
    classifications,
    pointPlaneIds,
    footprintOffset,
  );

  ctx.addArtifact('branch-score', 'metadata', metrics);
}
```

**Note on GT artifact routing**: `runner.js` injects `input-gt` and `input-bdtopo` as special input nodes. The wiring must route these to every `branch-score` instance. Since all instances share the same input nodes, this is automatic via the declarative wiring resolver — `input-gt` is a global artifact visible to all steps.

### 3.4. Benchmark.js integration

After `runPipeline()`, benchmark.js currently extracts only the pipeline's `outputs` for scoring. For v7-compare, it should also:

1. Collect all `branch-score#N` artifacts from the pipeline context
2. Print a summary table of all branch scores
3. Optionally store per-branch scores in bench.db (new column or JSON field in `runs`)

```
--- v7-compare scores (quick set avg) ---
Branch                  | Composite | SegF1 | Coverage | Residual
Axis 5: Strip Consensus |     91.2  | 0.89  |   0.94   |  0.032
Axis 9: Edge-First      |     87.4  | 0.85  |   0.91   |  0.038
Axis 8: Noise RANSAC    |     86.1  | 0.83  |   0.93   |  0.029
Hemisphere (main V7)    |     83.4  | 0.81  |   0.88   |  0.041
...
```

This is the key output that agents use to validate their implementation: "my branch scores roughly like the original research detector did."

### 3.5. UI visualization

The `branch-score` artifact should render in the artifact inspector as a formatted metrics table. The graph node for `branch-score#N` should display the composite score as a badge, similar to how output nodes currently show a 📊 icon.

---

## 4. Density Compensation Rules

Every new seeding step should apply density compensation **where it helps** and avoid it **where it hurts**. This is NOT a blanket "weight everything" rule — the ransac-full investigation (v6→v7) proved that indiscriminate weighting can be counterproductive.

### 4.0. The weight variance problem (CRITICAL — read first)

IGN LiDAR HD has extreme point-weight variance: dense in-band points (weight ~0.02) vs sparse inter-band points (weight ~4.6) — a **200× ratio**. This makes blanket weighting counterproductive in some operations:

**Where weights HELP**:
- **Spatial connectivity** (CC): sparse points genuinely need wider search radius to bridge scan-line gaps. Pair-radius pattern works well.
- **Size gates**: weighted area IS the correct metric for "is this surface physically large enough?" But use a **dual gate** (area ≥ threshold OR count ≥ minCount) because many legitimate CCs with hundreds of dense points have weighted area below 0.5 m².
- **Least-squares refit**: each point's influence on the fit should be proportional to the area it represents. Without weighting, dense scan-line clusters dominate the fit.
- **Histograms / KDE for grouping**: orientation and offset histograms benefit from area-weighted accumulation to prevent dense zones from dominating peaks.

**Where weights HURT**:
- **RANSAC sampling**: weighted sampling gives sparse inter-band points up to **180× more probability**. These points are **noisier** (edge of swath, higher measurement uncertainty). Plane hypotheses should be built from reliable dense measurements. **Use uniform sampling.**
- **RANSAC scoring (best-model selection)**: with weighted scoring, 5 sparse points (total weight ~23) beat 500 dense points (total weight ~5). This biases RANSAC toward garbage planes through sparse gap regions. **Use inlier count, not weighted sum.**

**Historical lesson**: ransac-full v5 used weighted sampling + weighted scoring throughout (the "fully density-compensated" approach). A point-weights indexing bug caused all weights to be NaN, which made the code fall back to uniform behavior. When the bug was fixed (v6), the true impact appeared: RANSAC found only 9 planes (down from 12 under NaN). Switching to uniform sampling + count-based scoring (v7) recovered **218 planes, 84% assigned** — invalidating the earlier conclusion that "coplanar cross-roof mixing is a fundamental RANSAC limitation."

**Rule for new steps**: Think about what each operation is trying to achieve. Weighting is correct for **physical area estimation** and **spatial bridging**. It is wrong for **hypothesis generation** (sampling) and **measurement agreement counting** (scoring). When in doubt, test both and compare.

### 4.1. Weighted accumulation — where appropriate

```javascript
// For histograms/KDE (orientation grouping, offset separation):
histogram[bin] += pointWeights[globalIdx];  // area-weighted

// For RANSAC scoring:
score += 1;  // COUNT, not weighted — "how many measurements agree?"
```

### 4.2. Weighted PCA / least-squares — always for refit

```javascript
// Weighted covariance for plane/surface refit:
for (const idx of indices) {
  const w = pointWeights[idx];
  const dx = x[idx] - wcx;  // weighted centroid
  cov[0] += w * dx * dx;
}
```

### 4.3. Size gates — dual gate pattern

```javascript
// Dual gate: area OR count (both must be checked)
const area = sumWeights(segment, pointWeights) / density;
const count = segment.length;
if (area < minSeedArea && count < minSeedCount) reject();
// This catches dense in-band surfaces that area gate alone would reject
```

### 4.4. Radius-based spatial operations (AP1/AP4 fix)

```javascript
// Fixed-radius search + minNeighbors guard (NOT fixed-K kNN for binary decisions)
const neighbors = radiusSearch(tree, pt, radius);
if (neighbors.length < minNeighbors) { skip(); return; }  // insufficient evidence
```

For connected components, use the pair-radius pattern:

```javascript
// CC with density-adaptive bridging
const ccRadius = Math.max(
  connectDist,           // floor: bridges typical scan-line gaps (2.0m for IGN HD)
  connectRadiusScale * Math.sqrt(pointWeights[idx]) / Math.sqrt(density)
);
```

### 4.5. No global-scope binary decisions at tile scale (AP3 fix)

Never use tile-wide histogram peaks to make per-point decisions. If a decision needs global context, scope it to a local neighborhood or defer it to after footprint association.

### 4.6. Null-safe metadata access

LiDAR metadata (scan-angles, point-source-ids, return-numbers, number-of-returns) may be absent in synthetic test cases. Every step that reads metadata MUST handle the null case:

```javascript
const scanAngles = ctx.hasArtifact('scan-angles') ? ctx.getArtifact('scan-angles').data : null;
if (!scanAngles) {
  // Fallback: assume uniform noise model, or skip metadata-dependent logic
}
```

### 4.7. Existing step density audit (as of Mar 15, 2026)

Full audit of all 57 step files in `lib/pipeline/steps/`. This shows agents what patterns are already in use — copy these, not the research detectors.

#### Gold standard steps (full density compensation)

| Step | Version | Weighted accum. | Radius search | Area gates | Key pattern |
|------|---------|----------------|---------------|------------|-------------|
| `tile-signals` | v10 | Emits `point-weights` = `π×d_k²/k` | K=32 expandable | — | Source of truth for all weights. Spherical KDE for scan-line compensation. |
| `hemisphere-normals-density-aware-segment` | v5 | S1: `histogram[bin] += w[i]`, S2: weighted KDE, S3: weighted CC | Pair-radius CC | `minSegmentArea` (m²) | 3-stage density-aware cascade. Use this as the template for new segmentation steps. |
| `competitive-grow` | v9 | — | `radiusSearch` with per-point adaptive radius: `scale × sqrt(w[i]) / sqrt(density)` | — | `growRadius(gi)` function. Pair-radius rule: `max(R_self, R_neighbor)`. |
| `point-stealing` | v9 | — | Same adaptive radius pattern | — | `stealRadius(gi)` function. |
| `ransac-full` | **v7** | **Selective**: uniform sampling + count scoring (NOT weighted — see §4.0). Weighted LS refit. | Pair-radius CC: `ccRadius(gi) = scale × sqrt(w[i]) / sqrt(density)` with `connectDist` floor | **Dual gate**: `sumArea()` ≥ `minSeedArea` OR count ≥ `minSeedCount` | Pool give-back. Stall detection. **v6→v7 lesson**: weighted sampling+scoring found only 9 planes; uniform found 218. |
| `ransac-poly` | **v6** | **Selective**: uniform sampling + count scoring. Weighted LS refit (`fitPolyWeighted`). Unweighted plane comparison (RMS gate). | kNN + connectDist (no pair-radius yet) | **Dual gate** (area OR count) | Per-point perpendicular distance via local surface normal. |
| `normal-cascade-seed` | v11 | `sumArea(indices)` = `Σ w[gi] / density` | — | `minSeedArea` (m²) | Converts cascade segments to seed planes. Area gate filters small segments. |
| `planes-from-owner` | v2 | `sumArea(indices)` = `Σ w[gi] / density` | — | `minPlaneArea` (m²) | Final owner→planes converter. Globally-indexed point-weights since v2. |
| `ransac-seed` | v9 | `sumArea(indices)` | — | `minSeedArea` (m²) | Fallback: `count / density` when weights absent. |
| `planarity-watershed` | v2 | `sumWeights(indices, weights)` for basin area | `radiusSearch` for boundary (0.5m + minNeighbors=3) | `minBasinArea` (m²) | Weighted plane fits via `fitPlaneWeighted`. |

#### Key code patterns to copy

**Adaptive radius** (from `competitive-grow.js`):
```javascript
const pointWeights = ctx.hasArtifact('point-weights') ? ctx.getArtifact('point-weights').data : null;
const density = ctx.hasArtifact('lidar-density') ? ctx.getArtifact('lidar-density').data : 10;

function searchRadius(gi) {
  if (!pointWeights) return baseRadius;
  const w = pointWeights[gi] || (1 / density);
  return Math.min(radiusMax, radiusScale * Math.sqrt(w) / Math.sqrt(density));
}
```

**Area gate** (from `normal-cascade-seed.js`):
```javascript
function sumArea(indices) {
  if (!pointWeights) return indices.length / density;  // fallback
  let s = 0;
  for (const gi of indices) s += pointWeights[gi];
  return s / density;
}
// Use:
if (sumArea(segment) < minSeedArea) continue;  // skip small segments
```

**Weighted histogram accumulation** (from `hemisphere-normals-density-aware-segment.js`):
```javascript
for (const gi of buildingIndices) {
  const bin = computeBin(normals, gi);
  histogram[bin] += pointWeights ? pointWeights[gi] : 1.0;  // weighted, not += 1
}
```

**Pair-radius CC** (from `ransac-full.js`):
```javascript
function findConnectedComponentsAdaptive(indices, tree, positions, pointWeights, density, connectDist, ccScale, ccMax) {
  // For each pair of points (i, j) in kNN:
  // Accept connection if dist(i,j) <= max(connectDist, pairRadius(i), pairRadius(j))
  // where pairRadius(gi) = min(ccMax, ccScale * sqrt(w[gi]) / sqrt(density))
  // connectDist is the FLOOR — bridges typical IGN HD scan-line gaps (2.0m)
}
```

#### Steps with intentional count-based thresholds (not bugs)

| Step | Threshold | Why it's OK |
|------|-----------|-------------|
| `cascade-shared.js` (used by `normal-cascade-segment*`) | `minSegmentPts = 10` | Cascade segments are preliminary groupings. Area gating happens downstream in `normal-cascade-seed`. |
| `strip-region-grow` | `minRegionPts` | Per-strip isolation + local kNN reduces density variance within a strip. |

#### Steps that need NO density compensation (geometric/filtering only)

These steps operate on pre-filtered subsets or perform pure geometric operations with no size gates:
`compute-normals`, `extract-denoise`, `detect-boundary`, `wall-roof-detect`, `ridge-edge-fit`, `hull-extraction`, `ridge-clip`, `combine-edges`, `snap-edges`, `compute-wall-edges`, `close-corner-gaps`, `clip-to-footprint`, `obstacle-filter`, `resolve-overlaps`, `shape-fitting`, `lod2-simplify`, `scope-to-building`, `co-register`, `co-register-dsm`, `categorize-planes`, `fit-solar-panels`, `build-obstacle-records`, `compute-ortho-artifacts`, `refit-planes`

#### Null-safety pattern (all steps)

Every step that reads optional artifacts uses this pattern:
```javascript
const pointWeights = ctx.hasArtifact('point-weights') ? ctx.getArtifact('point-weights').data : null;
// Then every use guards:
const w = pointWeights ? pointWeights[gi] : (1 / density);
```

This ensures backward compatibility with test cases that lack point-weights (shouldn't happen in V7, but defensive).

---

## 5. The 14 Segmentation Chains

### Two families

**Pure-thesis chains (T1-T5)**: raw algorithms derived from the original 4-axis mathematical theses BEFORE they converged to the standard 3-phase decomposition. These stay true to their root idea — agents must NOT bolt on normal histograms, offset histograms, or spatial CC from other approaches. If the thesis can't solve a case, document WHY and improve WITHIN the thesis. Full algorithmic detail in companion document `2026-03-15-five-pure-thesis-chains.md`.

**10-axis chains (A5-A9)**: from the 10-axis research session. These already converged but represent genuinely different signals (strip metadata, scan-angle noise, multi-return physics, boundary-first flow). Implement as proper steps with density improvements.

**Existing chains (RF, RP, PW, HM)**: already implemented, just need `planes-from-owner → branch-score` appended.

---

### 5.1. CHAIN T1 — Pure Radon Tomography

**Thesis**: "A plane in 3D is a delta function in Radon space. Project along 819 hemisphere directions, build 1D histograms, detect peaks = planes. Ramp-filtered back-projection. NO spatial CC, NO normal clustering."

**New step**: `radon-tomography-seed` (1 step)
**Full algorithm**: see `2026-03-15-five-pure-thesis-chains.md` §T1

**Summary**: 819 Fibonacci directions → project points → 1D histograms → ramp filter (FFT) → peak detection → accumulate in (direction, offset) space → cluster peaks → assign points to planes.

**Key artifacts**: `radon-accumulator` (2D heatmap: direction × offset), `radon-profiles` (per-direction 1D histograms), `filtered-profiles` (after ramp filter)

**Agent rule**: do NOT add spatial CC splitting. Coplanar surfaces merging is what the thesis predicts. Document it.

**Expected score**: 60-80 (coplanar merge, no spatial separation)

---

### 5.2. CHAIN T2 — Pure Height-Image Edge Detection

**Thesis**: "The building is an IMAGE, not a point cloud. Rasterize to DSM, apply Sobel/Canny edge detection, Hough line transform for ridges/eaves, flood-fill between edges."

**New step**: `height-image-seed` (1 step)
**Full algorithm**: see `2026-03-15-five-pure-thesis-chains.md` §T2

**Summary**: rasterize to DSM → Sobel gradient → Canny NMS → Hough line detection → region growing between edge pixels → plane fit per region → back-project to point cloud.

**Key artifacts**: `dsm` (height image), `edge-image` (detected boundaries), `hough-lines` (line overlay), `region-image` (2D segmentation)

**Agent rule**: do NOT compute normals from the point cloud. Normal info comes from DSM gradient only.

**Expected score**: 65-85 (resolution-dependent, misses subtle slopes)

---

### 5.3. CHAIN T3 — Pure Graph Laplacian Eigensegmentation

**Thesis**: "Build a normal-gated k-NN graph. The Laplacian's eigenvectors are the natural vibration modes. Eigencuts happen at ridges. Eigenvalue gap tells you how many surfaces. No plane model."

**New step**: `spectral-eigen-seed` (1 step)
**Full algorithm**: see `2026-03-15-five-pure-thesis-chains.md` §T3

**Summary**: build k-NN graph → gate edges by normal angle → compute graph Laplacian → Lanczos eigendecomposition (K=20) → eigenvalue gap → K-means in spectral embedding → plane fit per cluster.

**Key artifacts**: `eigenvalues` (spectrum plot — roof complexity fingerprint), `fiedler-vector` (per-point heatmap), `spectral-embedding` (2D/3D scatter)

**Agent rule**: do NOT fall back to agglomerative clustering (that's what Axis 3 DID). Stay spectral. The key question: does the eigenvalue gap correctly predict the number of surfaces?

**Expected score**: 40-75 (depends critically on gateAngle; eigendecomposition may be unstable)

---

### 5.4. CHAIN T4 — Pure Curvature-First Surface Typing

**Thesis**: "Every point has principal curvatures (κ₁, κ₂) that classify its surface type: plane (0,0), cylinder (κ,0), sphere (κ,κ), cone, saddle. Classify FIRST, grow same-type regions, fit type-specific primitives."

**New step**: `curvature-type-seed` (1 step)
**Full algorithm**: see `2026-03-15-five-pure-thesis-chains.md` §T4

**Summary**: exact quadratic surface fitting per point → principal curvatures → classify type → type-homogeneous region growing (only merge same-type neighbors) → type-specific fitting (plane/Kasa cylinder/sphere/cone).

**Key artifacts**: `point-type` (type-colored point cloud — planar/cylindrical/spherical/conical/saddle), `kappa1`/`kappa2` (curvature heatmaps), `principal-directions` (direction field)

**Agent rule**: do NOT add normal histogram grouping. Surface type IS the grouping criterion. Do NOT collapse curves to "planar" because the scorer doesn't reward curves.

**Expected score**: 75-90 (most viable pure thesis; curvature is directly informative)

---

### 5.5. CHAIN T5 — Pure DCT/JPEG Structural Decomposition

**Thesis**: "A roof is a low-frequency object. JPEG's DCT naturally separates structure from noise. Quantize DCT coefficients — what survives IS the building's core structure."

**New step**: `dct-structure-seed` (1 step)
**Full algorithm**: see `2026-03-15-five-pure-thesis-chains.md` §T5

**Summary**: rasterize to DSM → divide into blocks (8×8 or 16×16) → 2D DCT per block → classify block by energy distribution (simple-plane / edge-zone / complex) → quantize like JPEG → IDCT to get simplified DSM → difference = structural edges → region growing between edges → plane fit → back-project.

**Key artifacts**: `dsm` vs `dsm-simplified` (side-by-side), `dsm-difference` (structural edges), `block-types` (block classification map), `block-energy-spectrum` (frequency heatmap per block)

**Agent rule**: do NOT compute point-cloud normals. The DCT on the DSM IS the analysis. Experiment with quantization strength as the simplification knob.

**Expected score**: 55-80 (novel, untested; block artifacts may hurt; multi-scale variant should help)

**Multi-scale variant**: 32×32 blocks (major planes) → 16×16 (sub-planes) → 8×8 (small features). Like JPEG progressive mode — coarse structure first, then details.

---

### 5.6. CHAIN A5 — Strip-Aware Consensus

**Source**: Axis 5, `research/.../axis-5-strip/detector.js` v1.0; existing steps `strip-region-grow` v2 + `strip-consensus-merge` v1
**10-axis score**: **93.0** overall (best), **97.4** planar, **98.9%** plane match, **88.7** curved

**Steps**: `strip-region-grow` (existing, upgrade) + `strip-consensus-merge` (existing, upgrade)

**Algorithm**:
1. **Strip separation**: group points by `pointSourceId` → identify distinct flight strips
2. **Per-strip region growing**: for each strip independently, grow planar regions from curvature-seeded points. Weighted PCA per strip. Radius-based neighbor search.
3. **Cross-strip consensus merge**: for each pair of regions from different strips, check normal agreement + offset agreement + spatial overlap. If consensus across ≥2 strips → confirmed plane.
4. Single-strip planes: planes detected in only one strip get lower confidence but are still emitted.

**Upgrades needed**:
- `strip-region-grow`: add weighted PCA, area-based thresholds, radius-based CC
- `strip-consensus-merge`: weighted voting (area-weighted consensus), area-based output gates

**Key artifacts**:
- `strip-assignments`: Uint16Array(N) — per-point strip ID — visualization: strip-colored point cloud
- `per-strip-regions`: Array of per-strip region maps — visualization: per-strip segmentation
- `consensus-votes`: Array of {planeId, stripCount, totalArea} — how many strips confirm each plane

**Density fixes vs original**:
- Weighted PCA per strip
- Area-based region thresholds
- Weighted consensus voting

**Reads from tile-signals**: `positions`, `building-indices`, `point-normals`, `point-weights`, `kd-tree`, `planarity`, `lidar-density`, `lidar-z-noise`
**Metadata needed**: `point-source-ids` (REQUIRED), `scan-angles` (optional, for per-strip noise model)

**Strengths**: best 10-axis scorer; multi-strip confirmation is physically grounded; noise artifacts appear in single strip only
**Weaknesses**: requires metadata (graceful degradation if absent); needs ≥2 strips per building (usually true for IGN HD)

---

### 5.7. CHAIN A6 — Curvature-First Classification

**Source**: Axis 6, `research/.../axis-6-curvature-first/detector.js` v0.10
**10-axis score**: 86.4 overall, 87.5% plane match, **93.3%** curved match (best curved)

**New step**: `curvature-classify-seed` (1 step)

**Algorithm**:
1. Read `curvatures-k16`, `planarity`, `point-normals`, `point-weights` from tile-signals
2. Per-point **curvature tensor** analysis: classify as planar (both κ ≈ 0), cylindrical (one κ ≈ 0), spherical (κ₁ ≈ κ₂), conical (κ₁/κ₂ ratio), saddle (opposite signs), or uncertain
3. **Type-specific region growing**:
   - Planar: normal consistency + offset (same as A4)
   - Cylindrical: axis direction consistency + radius consistency (Kasa circle fit in cross-section)
   - Spherical: center + radius consistency
4. **Scale-aware curvature thresholds** (v0.9): threshold adapts to local point spacing — sparse zones have noisier curvature estimates
5. **Curved surface merging**: merge adjacent curved regions with compatible parameters (axis angle, radius ratio)
6. Area-based filtering

**Key artifacts**:
- `curvature-classification`: Uint8Array(N) — per-point surface type (0=planar, 1=cylindrical, 2=spherical, 3=conical, 4=saddle, 5=uncertain)
- `curved-fit-params`: Array of {type, axis, radius, center, rms} per curved region

**Density fixes vs original**:
- Weighted curvature estimation (tile-signals already does this with spherical KDE + K-expansion)
- Scale-aware thresholds (point spacing from point-weights)
- Weighted region growing
- Area-based gates

**Reads from tile-signals**: `positions`, `building-indices`, `point-normals`, `point-weights`, `kd-tree`, `curvatures-k16`, `planarity`, `lidar-density`, `lidar-z-noise`
**Metadata needed**: none

**Strengths**: best curved match rate (93.3%); curvature classification before fitting prevents wasted RANSAC iterations on curved surfaces; Quonset detection works (91-95)
**Weaknesses**: noisy curvature on sparse zones (mitigated by scale-aware thresholds); complex type-specific growing logic

---

### 5.8. CHAIN A7 — Multi-Return Edge Detection

**Source**: Axis 7, `research/.../axis-7-multireturn/detector.js`
**10-axis score**: 79.4 overall, 82.1 planar, 92.8% plane match, 76.7 curved

**New step**: `multireturn-edge-seed` (1 step)

**Algorithm**:
1. Read `return-numbers`, `number-of-returns` from metadata artifacts
2. Identify **multi-return edge points**: `returnNumber[i] < numberOfReturns[i]` → physical beam split at height discontinuity (same physics as wall-roof-detect v12 bit 4)
3. Use multi-return points as **boundary seeds** (unlike A9 which uses normal gradient)
4. **Normal-consistency flood fill** from non-edge interior points, blocked by multi-return boundaries
5. Fallback: where multi-return coverage is sparse (common — not every edge gets hit), supplement with **normal gradient edge detection** (Axis 9 style) as secondary boundary source
6. **Weighted plane fit** per region, area-based gates

**Key artifacts**:
- `multireturn-edge-map`: Uint8Array(N) — per-point: 0=single-return, 1=first-of-multi, 2=last-of-multi — visualization: edge-colored point cloud
- `combined-edge-map`: Uint8Array(N) — multi-return edges + normal gradient edges merged

**Density fixes vs original**:
- Weighted flood fill thresholds
- Area-based region gates
- Radius-based CC for flood fill connectivity

**Reads from tile-signals**: `positions`, `building-indices`, `point-normals`, `point-weights`, `kd-tree`, `lidar-density`, `lidar-z-noise`
**Metadata needed**: `return-numbers` (REQUIRED), `number-of-returns` (REQUIRED)

**Strengths**: multi-return edges are physically measured discontinuities, immune to normal noise; complementary signal to all other approaches
**Weaknesses**: sparse coverage (not every edge gets multi-return); lowest thesis bonus (25.3%) — multi-return adds limited value alone; needs normal-gradient fallback

---

### 5.9. CHAIN A8 — Noise-Weighted RANSAC

**Source**: Axis 8, `research/.../axis-8-noise/detector.js` v0.7
**10-axis score**: 88.6 overall, 94.4 planar, 91.1% plane match, 82.8 curved; **91.7** all-synth (2nd best)

**New step**: `noise-ransac-seed` (1 step)

**Algorithm**:
1. Read `scan-angles` from metadata. Compute per-point noise: `σ_eff[i] = σ_z / cos(scanAngle[i] × π/18000)` (angles stored as ×100)
2. **RANSAC with per-point adaptive threshold**: inlier if `dist ≤ 3 × σ_eff[i]` (not fixed threshold). Steep scan angles allow larger errors. **Uniform sampling** (not weighted — see §4.0).
3. **Count-based scoring**: best plane = most inliers (not weighted sum — see §4.0 ransac-full lesson). The per-point threshold already accounts for noise quality.
4. **Weighted LS refit** (heteroscedastic WLS with `w = cos²(scanAngle)`). Refit IS weighted because it's a physical fit, not hypothesis selection.
5. **Pair-radius CC** with density-adaptive radius (same pattern as ransac-full)
6. **Dual size gate** (area ≥ minSeedArea OR count ≥ minSeedCount)
7. **Pool give-back**: rejected CC points return to pool (from ransac-full finding)
8. After planar extraction:
   a. **Cap-constrained hemisphere** fitting on residuals (Axis 8's best innovation): sphere center constrained to Y = cap plane height
   b. **Kasa algebraic circle** fit for cylinders (multi-axis: try Y, Z, X, PCA_e1, PCA_e2)
   c. **Cone apex** via iterative regression (30-step search using weighted centroid of top 5%)
9. **BIC model selection** per residual cluster: plane vs cylinder vs sphere vs cone

**Key artifacts**:
- `noise-model`: Float32Array(N) — per-point σ_eff — visualization: noise-colored point cloud
- `curved-surfaces`: Array of {type, params, rms, inlierIndices}

**Density approach** (selective, following §4.0):
- Sampling: **uniform** (not weighted — sparse points are noisier, not better candidates)
- Scoring: **count-based** (per-point adaptive threshold already compensates for noise)
- Refit: **weighted** by `cos²(scanAngle)` (physical noise model)
- CC: **pair-radius** pattern (spatial bridging benefits from weights)
- Size gate: **dual** (area + count)
- Pool: **give-back** (rejected CC points return)

**Reads from tile-signals**: `positions`, `building-indices`, `point-normals`, `point-weights`, `kd-tree`, `lidar-density`, `lidar-z-noise`
**Metadata needed**: `scan-angles` (REQUIRED for noise model; fallback: uniform σ_z, degrades to ransac-full-like behavior)

**Strengths**: physically grounded noise model; per-point adaptive threshold; cap-constrained hemisphere is elegant; 2nd best all-synth
**Weaknesses**: scan angle variation at IGN HD only ~6% at ±20° — practical noise-weighting benefit is modest; stochastic variance ±5 composite per run

---

### 5.10. CHAIN A9 — Edge-First Boundary Detection

**Source**: Axis 9, `research/.../axis-9-edge-first/detector.js` v0.2
**10-axis score**: 92.8 overall* (76.9 without thesis bonus), 94.8 planar, 92.5% plane match, 90.7 curved, 100% curved match

**New steps**: `edge-detect-segment` (1 step)

**Algorithm**:
1. Per-point: compute **normal gradient magnitude** — for each point, measure how fast the normal changes across its `radiusSearch(r)` neighborhood. **Weight by point-weights**: sparse-zone gradient estimates are penalized (prevents false edges in density voids).
2. **Edge classification**: gradient > adaptive threshold → edge point. Threshold = `edgeThresholdDeg` (10°), adaptive to local noise.
3. **Curved surface un-marking**: if `edge_ratio > 0.70` in local neighborhood → it's a curved surface where normals change rapidly, NOT a boundary. Remove edge flag.
4. **Normal-consistency flood fill**: grow regions from interior (non-edge) seeds, blocked by edge points. Extra constraint: normal consistency within region (prevents leaking through weak edges).
5. **Weighted plane fit** per region. Regions with RMS > `planeMaxResidual` → deferred as potential curved.
6. **Curved recovery** from edge points + deferred regions + remaining unassigned:
   - Multi-axis cylinder fitting (Kasa circle fit per axis: Y, Z, X, PCA_e1, PCA_e2)
   - Sphere fitting (algebraic Kasa 3D)
   - Cone fitting (regression-based)
   - Best surface selected by min RMS with 2×avg_plane_rms guard
7. Final unmatched assignment + area gates

**Key artifacts**:
- `normal-gradient`: Float32Array(N) — per-point gradient magnitude — visualization: gradient heatmap (bright = edge)
- `edge-map`: Uint8Array(N) — per-point: 0=interior, 1=edge, 2=curved — visualization: edge overlay
- `flood-fill-regions`: Int32Array(N) — per-point region ID from flood fill — visualization: region colors
- `curved-surfaces`: Array of {type, params, rms, inlierIndices}

**Density fixes vs original**:
- Step 1: weighted gradient (original: unweighted kNN gradient)
- Step 4: radius-based flood fill connectivity (original: `connectRadius = max(0.5, spacing × 2.5)` — already good, keep but ensure pair-radius for sparse zones)
- Step 5: weighted plane fit (original: unweighted)
- Area gates everywhere

**Reads from tile-signals**: `positions`, `building-indices`, `point-normals`, `point-weights`, `kd-tree`, `lidar-density`, `lidar-z-noise`
**Metadata needed**: none

**Strengths**: completely inverted approach (edges first, regions second); edge-map artifact is unique diagnostic; 100% curved match in research
**Weaknesses**: heavy thesis bonus dependency (score drops 15.9 without it); edge threshold is tuning-sensitive; edge detection in density voids produces false edges (density weighting should help)

---

### 5.11. CHAIN RF — RANSAC Full (existing)

**Source**: `lib/pipeline/steps/ransac-full.js` v6 (already in V7 as dead-end)
**Current V7 status**: dead-end, density-compensated, 12 planes / 66% on Dahouet

**No new steps needed.** Already produces `owner`, `seed-planes`, `plane-equations`.

**Chain**: `ransac-full#0 → competitive-grow#RF → point-stealing#RF → planes-from-owner#RF → branch-score#RF`

Note: ransac-full has its own internal grow pass. The shared competitive-grow + point-stealing provide additional refinement for fair comparison.

---

### 5.12. CHAIN RP — RANSAC Poly (existing)

**Source**: `lib/pipeline/steps/ransac-poly.js` v5 (already in V7 as dead-end)

**Special case**: ransac-poly produces `poly-surfaces` (curved) + `owner`, but its `owner` may need adaptation to feed the standard chain. May need a thin `ransac-poly-to-owner` adapter step, OR modify ransac-poly to also emit standard `seed-planes` + `plane-equations` from the planar surfaces it detects.

**Chain**: `ransac-poly#0 → [adapter?] → planes-from-owner#RP → branch-score#RP`

---

### 5.13. CHAIN PW — Planarity Watershed (existing)

**Source**: `lib/pipeline/steps/planarity-watershed.js` v2 (already in V7 as dead-end)

**Similar to ransac-full**: already produces segmentation artifacts. Needs `planes-from-owner → branch-score` appended.

**Chain**: `planarity-watershed#0 → competitive-grow#PW → point-stealing#PW → planes-from-owner#PW → branch-score#PW`

---

### 5.14. CHAIN HM — Hemisphere Main Path (reference)

**Source**: V7 main pipeline
**Current score**: 83.4 avg on quick set

**Chain**: `hemisphere-normals-density-aware-segment#0 → normal-cascade-seed#0 → competitive-grow#HM → point-stealing#HM → planes-from-owner#HM → branch-score#HM`

This is the reference branch. All other branches are compared against it.

---

## 6. Implementation Plan

### 6.1. Phase 0 — Foundation (MUST be done first, single agent)

**Deliverables**:
1. `lib/pipeline/steps/branch-score.js` — the scoring step
2. `v7-compare` pipeline definition in `pipelines.js` — initially with only the 3 existing dead-ends (RF, RP, PW) + hemisphere main, each followed by `planes-from-owner → branch-score`
3. Benchmark.js integration: collect and display `branch-score#N` artifacts
4. Wire existing chains, verify all 4 initial branches produce scores on quick set
5. Add `branch-score` to `STEP_GROUPS` (new group: `scoring`)
6. Verify: running `node scripts/benchmark.js --pipelines v7-compare --test-cases quick` shows per-branch scores

**Verification**: The hemisphere branch score on v7-compare should match the v7 pipeline score (same segmentation, same scorer). ransac-full and planarity-watershed get scores for the first time.

### 6.2. Phase 1-5 — Parallel chain implementation (one agent per chain)

After Phase 0 is stable, each chain can be implemented **independently by a separate agent**. Each agent:

1. Creates `lib/pipeline/steps/{step-name}.js` with proper meta + fn
2. Adds the step to `STEP_GROUPS` in `pipelines.js`
3. Adds the chain to the `v7-compare` pipeline (steps + wiring overrides)
4. Adds any needed wiring overrides to `PIPELINE_OVERRIDES`
5. Runs the benchmark and verifies the branch score is reasonable
6. Creates a journal entry documenting the implementation

**Suggested agent parallelization** (7 parallel agents):

| Agent | Chains | New steps | Complexity | Notes |
|-------|--------|-----------|------------|-------|
| Agent 1 | T1 (Radon Tomography) | 1 | Medium | Needs FFT for ramp filter. Pure thesis — no spatial CC. |
| Agent 2 | T2 (Height-Image) + T5 (DCT/JPEG) | 2 | Medium | Both DSM-based — share rasterization. Image processing focus. |
| Agent 3 | T3 (Spectral Eigenseg) | 1 | High | Needs sparse Lanczos eigensolver (~50 lines). Fragile — gateAngle tuning critical. |
| Agent 4 | T4 (Curvature-First) | 1 | High | Richest feature set. Type-specific fitting. Must resist converging to normal+offset. |
| Agent 5 | A5 (Strip) + A7 (MultiReturn) | 2 upgrades | Medium | Metadata-dependent (PSID, returnNumber). Steps already exist. |
| Agent 6 | A6 (Curvature-First 10-axis) + A8 (Noise RANSAC) | 2 | Medium | Both use tile-signals curvatures/scan-angles. A8 uses selective density (§4.0). |
| Agent 7 | A9 (Edge-First) | 1 | Medium | Boundary-first flow. Normal gradient + flood fill. |

### 6.3. Conflict management for parallel agents

**The problem**: Multiple agents modify `pipelines.js` concurrently. Agent A's chain may break if Agent B introduces a bug in a shared step or changes wiring conventions.

**Rules for parallel agents**:

1. **Each agent owns its step files only.** Never modify another agent's step files. Shared steps (competitive-grow, point-stealing, planes-from-owner, branch-score) are frozen after Phase 0.

2. **Pipeline registration is append-only.** Each agent adds its chain to the `v7-compare` steps array and its wiring overrides to `PIPELINE_OVERRIDES['v7-compare']`. These are additive — one agent's additions don't break another's.

3. **If the benchmark breaks, check if it's your fault first.** Run your chain in isolation:
   ```bash
   node scripts/benchmark.js --pipelines v7-compare --test-cases quick --verbose 2>&1 | grep 'branch-score#YOUR_N'
   ```
   If YOUR branch scores look correct but the overall run fails, another agent may have introduced a bug. Wait for them to fix it.

4. **Use `--test-cases quick` during development.** Only run `--test-cases all` for final validation. Quick set (7 cases) takes seconds; all set (219 cases) takes minutes.

5. **Version your step.** Every new step starts at `version: 1`. Bump on every behavioral change. This is tracked in bench.db.

6. **Don't modify tile-signals.** It's the shared foundation. If you need a new artifact, compute it inside your step.

7. **Don't modify the scorer (lib/metrics/).** branch-score calls it as-is. If you think the scorer is wrong, file it as a journal finding, don't fix it in-flight.

8. **Git strategy**: each agent works on a feature branch (`feat/arena-axis-N`). Merge to main after verification. Merge conflicts in `pipelines.js` are resolved by the last merger — they're always additive (append to arrays).

### 6.4. Per-step implementation checklist

For each new seeding step, the implementing agent must:

- [ ] Create `lib/pipeline/steps/{name}.js` with `meta` (name, version, description, params, artifacts.reads, artifacts.writes) and `fn(ctx, params)`
- [ ] `meta.artifacts.writes` includes `owner` (type: `array`), `seed-planes` (type: `metadata`), `plane-equations` (type: `metadata`) at minimum
- [ ] `meta.artifacts.reads` lists all tile-signals artifacts consumed + any metadata artifacts
- [ ] Handle null metadata gracefully (scan-angles, point-source-ids, etc.)
- [ ] Apply all density rules from §4 (weighted accumulation, weighted PCA, area gates, radius CC)
- [ ] Produce at least one intermediate visualization artifact unique to the approach
- [ ] Add step name to `STEP_GROUPS` in `pipelines.js`
- [ ] Add chain to `v7-compare` pipeline steps array (seeding + grow + steal + planes-from-owner + branch-score)
- [ ] Add wiring overrides to `PIPELINE_OVERRIDES['v7-compare']` if needed
- [ ] Run `node scripts/benchmark.js --pipelines v7-compare --test-cases quick` — verify branch score is non-null and reasonable (composite > 50 for GT cases)
- [ ] Create journal entry `2026-03-XX-arena-chain-{name}.md`

### 6.5. Expected timeline

Phase 0 (foundation): ~2-4 hours
Each chain: ~2-4 hours
With 5 parallel agents: all chains done in ~1 day after Phase 0

---

## 7. Success Criteria

### 7.1. Per-branch

Each branch should produce:
- Non-null composite scores on all GT test cases
- Composite scores within ±20 points of the original research detector's score (accounting for density improvements, different test set, different scorer version)
- At least one unique intermediate artifact visible in the UI
- No regressions on the hemisphere main path score

### 7.2. Cross-branch

The arena should reveal:
- Which approach handles which roof type best (flat, gable, hip, mansard, dormer, L-shaped, multi-section)
- Which density improvements matter most (some approaches may not benefit from weighting)
- Whether the shared grow+steal chain consistently improves or sometimes hurts raw seeding quality
- Whether combined density+noise weight (A8) outperforms density-only weight

### 7.3. Pipeline-level

- `v7-compare` runs on the full test set without errors
- Per-branch scores accessible in benchmark output
- Total runtime: <10min on quick set (7 cases × 14 branches × ~5s each ≈ 8 min target; T3 eigendecomposition may be slower)
- All 219 test cases: <90 min

---

## 8. Reference Files

### 8.1. Research detectors (read-only reference — do NOT wrap these, write proper steps inspired by them)

| Axis | File |
|------|------|
| 1 | `research/2026-03-06-next-research-session/axis-1-radon/detector.js` |
| 2 | `research/2026-03-06-next-research-session/axis-2-multiview/detector.js` |
| 3 | `research/2026-03-06-next-research-session/axis-3-spectral/detector.js` |
| 4 | `research/2026-03-06-next-research-session/axis-4-curvature/detector.js` |
| 5 | `research/2026-03-06-next-research-session/axis-5-strip/detector.js` |
| 6 | `research/2026-03-06-next-research-session/axis-6-curvature-first/detector.js` |
| 7 | `research/2026-03-06-next-research-session/axis-7-multireturn/detector.js` |
| 8 | `research/2026-03-06-next-research-session/axis-8-noise/detector.js` |
| 9 | `research/2026-03-06-next-research-session/axis-9-edge-first/detector.js` |
| 10 | `research/2026-03-06-next-research-session/axis-10-density/detector.js` |

### 8.2. 4-axis research detectors (earlier session, different interface)

| Axis | File |
|------|------|
| 1 | `research/2026-03-06-four-axis-plane-detection/axis-1-radon/detector.js` |
| 2 | `research/2026-03-06-four-axis-plane-detection/axis-2-multiview/detector.js` |
| 3 | `research/2026-03-06-four-axis-plane-detection/axis-3-spectral/detector.js` |
| 4 | `research/2026-03-06-four-axis-plane-detection/axis-4-curvature/detector.js` |

### 8.3. Existing pipeline steps (shared infrastructure)

| Step | File | Version |
|------|------|---------|
| tile-signals | `lib/pipeline/steps/tile-signals.js` | v10+ |
| competitive-grow | `lib/pipeline/steps/competitive-grow.js` | v9 |
| point-stealing | `lib/pipeline/steps/point-stealing.js` | v9 |
| planes-from-owner | `lib/pipeline/steps/planes-from-owner.js` | v2 |
| ransac-full | `lib/pipeline/steps/ransac-full.js` | v6 |
| ransac-poly | `lib/pipeline/steps/ransac-poly.js` | v5 |
| planarity-watershed | `lib/pipeline/steps/planarity-watershed.js` | v2 |
| hemisphere-normals-density-aware-segment | `lib/pipeline/steps/hemisphere-normals-density-aware-segment.js` | v5 |
| normal-cascade-seed | `lib/pipeline/steps/normal-cascade-seed.js` | v? |
| strip-region-grow | `lib/pipeline/steps/strip-region-grow.js` | v2 |
| strip-consensus-merge | `lib/pipeline/steps/strip-consensus-merge.js` | v1 |

### 8.4. Shared geometry utilities

| File | Key functions |
|------|---------------|
| `lib/geometry/kd-tree.js` | `buildKdTree`, `kNearest`, `radiusSearch` |
| `lib/geometry/pca.js` | `normalFromCovariance`, `fitPlane`, `refineNormal`, `covarianceMatrix` |
| `lib/geometry/primitives.js` | `pointToPlaneDistance`, `angleBetweenNormals`, `createRng` |
| `lib/geometry/histogram.js` | histogram utilities |
| `lib/geometry/lambert.js` | Lambert azimuthal equal-area projection |
| `lib/geometry/curved-primitives.js` | Kasa circle fit, sphere fit, cone fit |
| `lib/algorithms/_common.js` | `ransacPlane`, `ransacPlaneWeighted`, `fitPlaneLeastSquares`, `createPlaneResult`, `findConnectedComponents`, `clusterPointsXZ`, `stealPoints` |

### 8.5. Scoring

| File | Purpose |
|------|---------|
| `lib/metrics/index.js` | `computeAllMetrics()` — main entry point |
| `lib/metrics/segmentation.js` | F1 score via greedy matching |
| `lib/metrics/composite.js` | `compositeScoreV3()` — weighted composite |
| `scripts/benchmark.js` | CLI benchmark orchestration |

### 8.6. Journal entries (context)

| Entry | Key content |
|-------|-------------|
| `2026-03-13-v7-master-plan.md` | V7 architecture, density anti-patterns AP1-AP4, task list |
| `2026-03-07-ten-axis-research-session-report.md` | Authoritative cross-axis scores, convergent techniques |
| `2026-03-06-four-axis-research-synthesis.md` | Axes 1-4 deep dive |
| `2026-03-12-local-density-compensation.md` | Density bias analysis, point-weights design |
| `2026-03-15-ransac-full-density-compensation.md` | Pool give-back, pair-radius CC, grow pass |
| `2026-03-15-ransac-poly-curved-surface-detection.md` | Polynomial RANSAC, perpendicular distance, Hessian classification |
| `2026-03-13-cascade-ridge-detect-methods-1-2-retirement.md` | Anti-patterns AP1, AP3 case study |
