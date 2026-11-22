---
id: "2026-03-13-v7-master-plan"
date: 2026-03-13
title: "V7 Master Plan: Tile-First Density-Aware Roof Reconstruction"
tags: [v7, architecture, density, tile-wide, master-plan, pipeline, migration, anti-patterns]
category: design
status: proposed
refs:
  - "2026-03-12-local-density-compensation"
  - "2026-03-12-tile-signals-unified-pca"
  - "2026-03-12-density-aware-planarity-watershed"
  - "2026-03-12-spherical-kde-scan-line-compensation"
  - "2026-03-11-footprint-snap-strategy-redesign"
  - "2026-03-07-ten-axis-research-session-report"
  - "2026-03-13-cascade-ridge-detect-methods-1-2-retirement"
pipeline: v7
---

## 1. Historical Context: Why V7 Exists

### The exploration pipeline — V6's kitchen sink

The `exploration` pipeline was V6's accumulation of everything: every prior pipeline generation (V2 RANSAC baseline, V3 categorization, V4 normal histogram, V5 multimodal, V6 cascade + all KDE variants) wired as dead-end branches for side-by-side UI comparison, plus partial integrations of 10-axis research results (Axis 5 strip-consensus, Axis 10 planarity watershed), co-registration experiments (ICP, directional, DSM), boundary edge detection (Hough, kNN-PCA), snap-footprint, ortho artifacts, and solar panel fitting.

It became the single place to visually compare everything, with ~50 step instances across 8+ branches feeding one main output path (building-scoped v6g-kde-full chain).

### Why exploration can't become V7

Two fundamental discoveries made the exploration architecture untenable:

1. **Density bias is pervasive.** The `2026-03-12-local-density-compensation` analysis showed that almost every counting-based operation in the cascade (histogram accumulation, KDE amplitudes, spatial clustering, min-segment thresholds, plane fits) is biased by non-uniform LiDAR density (beam geometry 2:1, strip overlap 2×, occlusion shadows). Retrofitting `point-weights` into the existing cascade-shared.js plumbing — while preserving backward compatibility with 15 V6 variants — would be a maintenance nightmare.

2. **Tile-first is structurally incompatible with building-first.** The exploration pipeline starts with tile-signals (tile-wide), then immediately scopes to a building via `scope-to-building` + `extract-denoise` for the main v6g chain. Tile-wide segmentation and building-scoped segmentation produce fundamentally different `owner` arrays, `building-points` sets, and index spaces. Running both in the same pipeline creates a tangled mess of instance numbering and artifact routing.

### The decision: fresh start + planned migration

V7 starts from a clean tile-wide, density-aware foundation:
- `tile-signals` as the single preprocessing step (replaces `extract-denoise` + `compute-normals`)
- `hemisphere-normals-density-aware-segment` as the primary segmenter (replaces all cascade-shared.js variants)
- `point-weights` flowing through every operation
- No footprint crop until after segmentation

**But we don't throw away exploration's work.** Every step and branch that existed in exploration needs to be evaluated for V7 re-introduction — either as:
- A **production step** in the main V7 chain (if it serves the master plan)
- A **dead-end visualization branch** (for comparison, debugging, idea validation)

The conditions for re-introduction:
1. **Wiring compatibility** — can it consume tile-signals artifacts efficiently, or does it need its own kd-tree / normals / preprocessing?
2. **Density resilience** — does it use raw point counts anywhere? If so, it needs point-weights integration.
3. **Scope correctness** — does it operate at the right scope for its position in the pipeline? (tile-wide before footprint association, building-scoped after)

---

## 2. Design Principles

Three foundational shifts distinguish V7 from all prior pipelines:

| Principle | V2–V6 (old) | V7 (new) |
|-----------|-------------|----------|
| **Scope** | Crop to building footprint early (`extract-denoise`) | Process entire tile until segmentation is done |
| **Density** | Every point = 1 vote | Every point weighted by Voronoi cell area (`point-weights`) |
| **Footprint role** | Trusted input (crop boundary, edge positions) | Weak topological hint (relative angles only); real geometry comes from LiDAR |

### Why tile-first

Cropping to a footprint before segmentation creates three problems:
1. **Information loss** — points outside the footprint (neighboring buildings, shared walls, ground context) are discarded before any algorithm sees them.
2. **Footprint error propagation** — BDTOPO polygons have 1–3 m positional errors. Cropping with a wrong boundary clips valid building points and includes non-building points.
3. **Density artifacts at crop boundary** — kNN neighborhoods near the crop edge are asymmetric, biasing normals, planarity, and curvature toward the interior.

### Why density compensation

IGN LiDAR HD has three systematic density non-uniformities:
- **Beam geometry**: 2:1 nadir-to-swath-edge ratio within a single strip
- **Strip overlap**: 2× density in overlap zones, sharp discontinuity at overlap edge
- **Occlusion shadows**: half-density or void behind vertical features

Every counting-based operation is biased. The `point-weights` artifact (`w_i = π·d_k²/k`) normalizes all operations to physical surface area. Area-based thresholds (`minSegmentArea`, `minBasinArea`) replace point-count thresholds. Plane fits use weighted covariance.

---

## 3. Pipeline Architecture — Stages A through I

```
┌─────────────────────────────────────────────────────────────────────┐
│  STAGE A — TILE-WIDE SIGNAL COMPUTATION                            │
│  tile-signals                                                       │
│  → eigenvalues, normals, curvatures, planarity, linearity,          │
│    scattering, point-weights, kd-tree, reliability                  │
├─────────────────────────────────────────────────────────────────────┤
│  STAGE B — TILE-WIDE SEGMENTATION                                   │
│  Primary: hemisphere-normals-density-aware-segment                  │
│    S1: Lambert 128² (weighted) → S2: offset KDE → S3: radius CC    │
│  Dead-ends: planarity-watershed (Axis 10), strip-consensus (Axis 5) │
├─────────────────────────────────────────────────────────────────────┤
│  STAGE C — PLANE REFINEMENT (tile-wide)                             │
│  cascade-seed → grow → ransac → grow → steal → refit               │
├─────────────────────────────────────────────────────────────────────┤
│  STAGE D — EDGE FITTING ON DETECTED PLANES                         │
│  ridge-edge-fit (3D) → project to XZ horizontal                    │
├─────────────────────────────────────────────────────────────────────┤
│  STAGE E — HORIZONTAL EDGE GRAPH                                    │
│  merge collinear → split at intersections → snap endpoints          │
├─────────────────────────────────────────────────────────────────────┤
│  STAGE F — FOOTPRINT ASSOCIATION (detect-then-match)                │
│  Hough+RANSAC boundary edges → DP match to footprint topology       │
│  → real footprint polygon per building                              │
├─────────────────────────────────────────────────────────────────────┤
│  STAGE G — BUILDING CLASSIFICATION & WALLS                          │
│  categorize roof/non-roof → find wall planes → flag edge types      │
│  → close watertight 3D shell                                        │
├─────────────────────────────────────────────────────────────────────┤
│  STAGE H — ORTHO-PHOTO CO-REGISTRATION                             │
│  3D model → synthetic edges → cross-correlate with orthophoto       │
├─────────────────────────────────────────────────────────────────────┤
│  STAGE I — OBSTACLE RESOLUTION & TEXTURE                            │
│  classify remaining points → confirm via ortho → texture map        │
└─────────────────────────────────────────────────────────────────────┘
```

### Current V7 pipeline (implemented)

```
tile-signals → planarity-watershed → hemisphere-normals-density-aware-segment
→ normal-cascade-seed → competitive-grow → ransac-seed → competitive-grow
→ point-stealing → refit-planes → ridge-edge-fit → detect-boundary
→ hull-extraction → wall-roof-detect → ridge-clip → combine-edges
→ snap-edges → compute-wall-edges → close-corner-gaps → clip-to-footprint
→ obstacle-filter → resolve-overlaps → shape-fitting → lod2-simplify
```

Benchmark: 7/7 pass, avg 83.4, ~25s/case (vs V6g-kde-full avg 90.9).

---

## 4. Stage Details

### Stage A — tile-signals (done, v9)

Single unified preprocessing replacing 3 former steps. One kd-tree, one kNN at K=32, sliced for K=8/16. 3D spherical KDE weighting (von Mises-Fisher) for scan-line anisotropy compensation. Dynamic K-expansion when spatial linearity is high. Produces 17 artifacts including `point-weights`, `planarity`, `linearity`, `scattering`, `eigen-reliability`.

### Stage B — Tile-wide segmentation (done)

**Primary: hemisphere-normals-density-aware-segment** — Lambert 128² equal-area accumulation (weighted) → per-group weighted offset KDE (Silverman) → radius-graph CC. No pole singularity, full 2D coupling, density compensation native.

**Dead-end: planarity-watershed (Axis 10, v2)** — watershed on planarity scalar field. Weighted plane fits, area-based thresholds, radius-bounded boundary detection.

### Stage C — Plane refinement (done)

`normal-cascade-seed → competitive-grow → ransac-seed → competitive-grow → point-stealing → refit-planes`. No cascade-ridge-detect (ridge-candidates defaults empty).

### Stage D — Edge fitting (partially done)

`ridge-edge-fit` finds 3D edges at plane-pair intersections. XZ projection not yet implemented.

### Stage E — Horizontal edge graph (not started)

Merge collinear edges, split at intersections, snap near-touching endpoints → clean 2D planar graph.

### Stage F — Footprint association (designed, partially implemented)

**detect-then-match** strategy (journal: `2026-03-11-footprint-snap-strategy-redesign`):
1. Hough + RANSAC edge detection on boundary candidates — no footprint knowledge (`boundary-edge-detect.js` exists, v3)
2. DP topology-aware matching: footprint edge sequence aligned to detected edges (angular compatibility, transition costs at corners)
3. Polygon reconstruction: replace matched edges, intersect for corners

The footprint's topology (relative edge angles) is reliable; position/lengths are not. Alternative kNN-PCA edge detection also exists (`boundary-edge-detect-knn-pca.js`, v3).

### Stage G — Building classification & walls (partially done)

`categorize-planes` and `wall-roof-detect` exist from V6. Need adaptation for tile-wide scope and density awareness.

### Stage H — Ortho-photo co-registration (research done)

DSM edge cross-correlation proven (Amiens: WECS 0.48→1.00). `co-register-dsm.js` exists. Extension to full 3D model planned.

### Stage I — Obstacles & texture (not started)

`build-obstacle-records`, `fit-solar-panels`, `compute-ortho-artifacts` exist from exploration. Need scope adaptation.

---

## 5. Migration from Exploration to V7

### 5.1. Inventory: what exploration has

The exploration pipeline contains ~50 step instances across these branches:

| Branch | Steps | Scope | Purpose | Main path? |
|--------|-------|-------|---------|------------|
| **tile-signals** | `tile-signals#0` | tile | Preprocessing: normals, eigenvalues, point-weights | Yes |
| **planarity-watershed** | `planarity-watershed#0` | tile | Axis 10 watershed segmentation | Dead-end |
| **Tile cascade** | `kde#0 → crd#0 → seed#0 → cg#0 → rs#0 → cg#1 → ps#0` | tile | Tile-wide cascade-KDE segmentation | Dead-end (feeds main path in v7) |
| **Co-registration** | `detect-boundary#0 → co-register#0 → co-register-directional#0 → co-register-dsm#0 → snap-footprint#0` | tile/building | 4 alignment methods compared side-by-side | Dead-end (3 variants), 1 feeds main path |
| **Boundary edge detect** | `boundary-edge-detect#0`, `boundary-edge-detect-knn-pca#0` | tile | Hough+RANSAC and kNN-PCA edge detection | Dead-end |
| **Scope transition** | `scope-to-building#0 → extract-denoise#0` | building | Crop to footprint after co-registration | Feeds building chain |
| **V2 baseline** | `ransac-seed#1` | building | Raw RANSAC without normals/cascade | Dead-end |
| **Axis 5** | `strip-region-grow#0 → strip-consensus-merge#0` | building | Per-strip growth + cross-strip consensus | Dead-end |
| **Building cascade (v6g)** | `compute-normals#0 → kde#1 → crd#1 → seed#1 → cg#2 → rs#2 → cg#3 → ps#1` | building | Building-scoped cascade-KDE segmentation | Main path |
| **V3 categorization** | `categorize-planes#0 → asymmetric-steal#0` | building | Roof/obstacle classification + asymmetric steal | Dead-end |
| **Progressive pass 2** | `kde#2 → crd#2 → seed#2 → cg#4` | building | Second cascade pass on residuals | Dead-end |
| **Hull + post-processing** | `refit → ridge-edge-fit → hull → wall-roof → ridge-clip → combine-edges → snap-edges → compute-wall-edges → close-corners → clip-to-footprint → obstacle-filter → resolve-overlaps → shape-fitting → lod2` | building | Full geometry extraction chain | Main path |
| **Final** | `build-obstacle-records#0 → compute-ortho-artifacts#0 → fit-solar-panels#0` | building | Obstacle records, ortho processing, solar panels | Main path |

### 5.2. Migration assessment per step

For each step currently in exploration but absent or different in V7:

#### Already in V7 (same or equivalent)

| Step | Status | Notes |
|------|--------|-------|
| `tile-signals` | **In V7** | Identical |
| `planarity-watershed` | **In V7** | Dead-end, identical |
| `hemisphere-normals-density-aware-segment` | **In V7** | Replaces tile cascade chain |
| `normal-cascade-seed` | **In V7** | Same step, tile-wide scope |
| `competitive-grow` | **In V7** | Same step, tile-wide scope |
| `ransac-seed` | **In V7** | Same step, tile-wide scope |
| `point-stealing` | **In V7** | Same step, tile-wide scope |
| `refit-planes` | **In V7** | Same step |
| `ridge-edge-fit` | **In V7** | Same step |
| `detect-boundary` | **In V7** | Same step |
| `hull-extraction` → `lod2-simplify` chain | **In V7** | Full post-processing chain |

#### Needs re-introduction as dead-end visualization

| Step | Scope | Density-aware? | Wiring to tile-signals? | Migration effort |
|------|-------|---------------|------------------------|------------------|
| `normal-cascade-segment-kde` (tile) | tile | **No** — uses raw `+= 1` | Reads `building-points`, `point-normals` from tile-signals — compatible | Low: add as dead-end after tile-signals. Density fix = pass `point-weights` to cascade-shared accumulators |
| `cascade-ridge-detect` (tile) | tile | **Partially** — v4 Method 3 uses radius search, but Methods 1 & 2 are fundamentally wrong (see `2026-03-13-cascade-ridge-detect-methods-1-2-retirement`) | Reads `cascade-segments` — compatible | Low: wire after tile cascade-kde. Override to kNN-only (Methods 1 & 2 disabled) |
| `strip-region-grow` (Axis 5) | building | **No** — raw point counts | Needs `building-points` from scope-to-building, not tile-signals | Medium: needs scope-to-building first, own kd-tree. Dead-end only |
| `strip-consensus-merge` (Axis 5) | building | **No** | Reads from strip-region-grow | Low: follows strip-region-grow |
| `normal-cascade-segment-kde` (building) | building | **No** | Needs scoped `building-points` + `compute-normals` | Medium: needs scope transition. Dead-end only |
| `ransac-seed` (V2 baseline) | building | **No** | Needs scoped `building-points` | Low: dead-end after scope transition |
| `categorize-planes` | building | **No** — uses footprint-based heuristics | Reads `owner`, `plane-equations`, `footprint` | Low: wire after main path or dead-end |
| `asymmetric-steal` | building | **No** | Reads from categorize-planes | Low: follows categorize-planes |
| Progressive pass 2 (kde#2 → cg#4) | building | **No** | Reads `owner` from asymmetric-steal | Medium: 4-step chain, dead-end only |

#### Needs re-introduction as production step

| Step | Target stage | Density-aware? | Adaptation needed |
|------|-------------|---------------|-------------------|
| `co-register` | Stage F | N/A (geometric) | Works on boundary candidates — density-neutral. But V7 may replace with detect-then-match. Keep as dead-end for now. |
| `co-register-directional` | Stage F | N/A (geometric) | Same. Dead-end for comparison. |
| `co-register-dsm` | Stage H | N/A (image-domain) | Works on rasterized DSM — inherently density-invariant. Production candidate for ortho co-registration. |
| `boundary-edge-detect` | Stage F | **No** — raw candidate counts for Hough votes | Should weight votes by `point-weights`. Production candidate. |
| `boundary-edge-detect-knn-pca` | Stage F | **No** — raw kNN | Dead-end for comparison with Hough approach. |
| `snap-footprint` | Superseded | — | Replaced by detect-then-match (Stage F). Keep as dead-end for comparison. |
| `scope-to-building` | Stage G+ | N/A (geometric crop) | Needed when transitioning from tile-wide to building-scoped steps. Production step. |
| `compute-ortho-artifacts` | Stage H | N/A (image-domain) | Production step. No density concern. |
| `build-obstacle-records` | Stage I | **No** | Needs adaptation for tile-wide input. Production step. |
| `fit-solar-panels` | Stage I | N/A (geometric) | Works on plane hulls — density-neutral. Production step. |
| `wall-roof-detect` | Stage G | **Partially** — uses density for gap threshold | Already auto-tunes from `lidar-density`. May need point-weights for candidate weighting. |
| `categorize-planes` | Stage G | **No** | Production candidate after footprint association is done. Needs real footprint, not BDTOPO. |

### 5.3. Exploration steps NOT needed in V7

| Step | Reason |
|------|--------|
| `extract-denoise` | Replaced by `tile-signals` (tile-wide) + `scope-to-building` (building-wide) |
| `compute-normals` | Replaced by `tile-signals` (normals computed once for all) |
| `normal-histogram-seed` (V4) | Superseded by hemisphere segmentation |
| `histogram-seed` (V3) | Superseded |
| `flat-detect` (V3) | Superseded |
| `parallel-edge-clip` | Replaced by `ridge-clip` |
| `sharp-edges` | Replaced by `ridge-clip` |

---

## 6. 10-Axis Research Integration

The 10-axis research session (Mar 6-7, ~7.5h, 10 independent subagents, 23-metric scoring) explored 10 radically different algorithm families on the same test set.

### 6.1. Axes already integrated

| Axis | Quick Score | What V7 takes | Where | Status |
|------|-------------|---------------|-------|--------|
| 10 | 86.9 | Planarity watershed | Stage B dead-end | **Done** (v2, density-aware) |
| 5 | 93.0 | Strip-consensus | Exploration dead-end | Needs V7 migration |

### 6.2. Axes to integrate as dead-end branches

| Axis | Quick Score | Key idea | V7 insertion point | Prerequisites |
|------|-------------|----------|-------------------|---------------|
| 5 | 93.0 | Per-strip region grow + cross-strip consensus merge | After `scope-to-building` (building-scoped) | Needs PSID metadata, own kd-tree. Not density-aware — accept as-is for comparison |
| 9 | 92.8* | Normal gradient edge detection → flood fill between edges | After `tile-signals` (tile-wide) | Reads `point-normals`, `point-weights` from tile-signals. Edge detection is inherently boundary-based — density less critical |
| 8 | 88.6 | Noise-weighted RANSAC with per-point σ from scan angle | After `tile-signals` (tile-wide) | Reads `point-normals`. The noise model (cos²(scanAngle)) is complementary to point-weights |
| 1 | 84.5 | CT/Radon 810-direction hemisphere sweep | After `tile-signals` (tile-wide) | Heavy computation (810 projections). Pure geometry — no density concern |
| 6 | 86.4 | Curvature-first classification before fitting | After `tile-signals` (tile-wide) | Reads `curvatures-k16`, `planarity`. Already uses eigenvalue-derived features |
| 2 | 80.1 | Multi-view rasterization + CC detection | After `tile-signals` (tile-wide) | Rasterization is inherently density-invariant |
| 4 | 79.3 | PCA curvature + region growing | After `tile-signals` (tile-wide) | Similar to Axis 6 |
| 3 | 66.9 | Graph Laplacian spectral methods | After `tile-signals` (tile-wide) | Reads `point-normals`. Binary gating on normal similarity |
| 7 | 79.4 | Multi-return edge detection | After `tile-signals` (tile-wide) | Reads multi-return metadata (returnNumber, numberOfReturns) |

*Score includes 15-point thesis bonus

### 6.3. Convergent techniques to adopt

These were independently discovered by 2+ axes — canonical for V7:

| Technique | Axes | Where in V7 |
|-----------|------|-------------|
| Kasa algebraic circle fit | 4, 5, 8, 9, 10 | Curved surface recovery (Stage C or post) |
| Fragment-to-sphere detection | 1, 3, 7, 8 | Curved surface recovery |
| Base-plane-constrained hemisphere | 1, 7, 8 | Dome/silo detection |
| Cone apex via linear regression | 4, 9, 10 | Cone spire fitting |
| Normal-based region splitting | 5, 7, 9 | Core of Stage B1 S1 (already there) |

### 6.4. Unsolved across all axes

- Ogive domes — no revolution surface fitting
- Silo + cone composites — cylinder/cone transition
- Small cone spires — too few points
- Polygonal silos — faceted surfaces confuse curvature
- Row houses — identical adjacent slopes separated only by physical gap

---

## 7. Key Design Decisions

### Lambert over HEALPix

No external library needed, sub-pixel bilinear interpolation, natural pole handling, equal-area preserved. 128×128 = 16,384 bins vs HEALPix N_side=16 = 1,536.

### No early footprint crop

Segmentation runs on all tile points. Footprint introduced only for matching (Stage F) and classification (Stage G).

### Detect-then-match over project-based snapping

snap-footprint v5 (per-edge projection) hit a structural limit: BDTOPO position determines which candidates each edge sees. detect-then-match inverts this: find edges from LiDAR (Hough+RANSAC), then match to footprint topology via DP sequence alignment. Footprint constrains matching, not detection.

### Area-based thresholds everywhere

`minSegmentArea` replaces `minSegmentPts`. Histogram bins accumulate `weight[i]` not `+1`. Plane fits use weighted covariance. Pipeline becomes density-invariant.

---

## 8. Density Anti-Patterns

Lessons learned from V6 and the 10-axis research. These are patterns that **must not** appear in V7 production steps. See also `2026-03-13-cascade-ridge-detect-methods-1-2-retirement` for the case study that motivated AP1 and AP3.

### AP1 — Fixed-K kNN for spatial proximity decisions

**Never use fixed-K nearest neighbors to decide local binary properties (ridge/not-ridge, boundary/interior, same-surface/different-surface) in non-uniform density point clouds.**

K nearest neighbors defines an adaptive-radius ball whose size is inversely proportional to local density. In sparse zones (beam shadows, swath edges, occlusion corridors), the ball grows large enough to include physically distant points from unrelated surfaces. The algorithm then makes a false-positive decision based on neighbors that are not in the point's physical neighborhood.

**Example**: `cascade-ridge-detect` Method 3 (v2) used K=8 nearest neighbors to check if a point borders two segments. In a beam shadow at 8 pts/m², the 8th neighbor was 2–3 m away — across a physical gap onto a different roof plane. Interior points were falsely flagged as ridges.

**Fix**: Use `radiusSearch(fixed_radius)` + `minNeighbors` guard.
- The radius defines a physical neighborhood independent of density.
- The `minNeighbors` guard handles sparse zones: if fewer than N points are within the radius, the evidence is insufficient — **skip rather than guess**.
- This is the pattern used in `cascade-ridge-detect` v4 (radius=0.5m, minNeighbors=3) and `planarity-watershed` v2 (boundaryRadius=0.5m, minBoundaryNeighbors=3).

**Where it still lurks**: any step using `kNearest()` for a binary spatial decision. Steps using kNN for aggregation (PCA, KDE, plane fitting) are less affected because outlier neighbors are diluted by the majority — but they still benefit from density weighting (see AP2).

### AP2 — Unweighted accumulation (every point = 1 vote)

**Never accumulate `+= 1` per point in histograms, KDE, or threshold checks. Accumulate `+= weight[i]` where weight is the point's Voronoi cell area.**

Dense zones (strip overlap, nadir) contribute more votes per unit area, biasing peak detection, KDE amplitudes, and minimum-size thresholds toward high-density regions. A 20 m² sparse plane at 30 pts/m² (600 points) produces a weaker peak than a 5 m² dense zone at 200 pts/m² (1000 points).

**Where it still lurks**: `cascade-shared.js` Stage 1 histogram (`histogram[bin]++`), Stage 1 KDE (`eligTilts.push()`), Stage 2 histogram (`offsetBins += 1`), Stage 2 KDE (`gaussianKDE(offsets)`). All fixed in the hemisphere segmenter but not in the cascade code.

### AP3 — Global-scope property checks at tile scale

**Never use global statistical properties (histogram peaks, orientation clusters) to make per-point decisions at tile scope.**

At building scope, the orientation histogram has 2–6 peaks representing roof planes of one building. A point near 2+ peaks is plausibly at a ridge. At tile scope, the histogram has dozens of peaks from all buildings in the tile. A point near 2+ peaks may simply be on building A while building B across the tile happens to have a similar orientation.

**Example**: `cascade-ridge-detect` Method 1 (normal-space) checks if a point's tilt/azimuth is within margin of 2+ orientation peaks. At tile scope, this flags vast numbers of false positives — any point whose orientation falls between any two peaks anywhere in the tile.

**Example**: `cascade-ridge-detect` Method 2 (offset-space) is even more fundamentally wrong. It flags points whose perpendicular offset falls between 2+ offset peaks within an orientation group. But two parallel planes at different heights form a step/wall, not a ridge. A point between the offset peaks is ambiguously assigned, not at a boundary between differently-oriented planes. This method is conceptually confused about what a ridge is — it was wrong at building scope too, just less visibly.

**Fix**: Local spatial checks only (Method 3 pattern). If a global property is needed, derive it per-building after footprint association (Stage G), not per-tile.

### AP4 — Grid-based spatial clustering with fixed cell size

**Fixed-cell flood-fill (`clusterPointsXZ` with 1m cells) fractures continuous planes at density voids.**

A beam shadow or narrow occlusion corridor creates an empty cell row. The 4-connected flood-fill cannot bridge it. Either half of the plane may fall below `minSegmentPts` and be discarded. This is especially damaging for L-shaped or T-shaped planes where a single scan-line gap bisects the narrow section.

**Fix**: Radius-graph connected components (Stage B1 S3) or bridging tolerance in flood-fill (allow 1-cell gap jumps when adjacent occupied cells have high weight = sparse zone).

---

## 9. Task List

### Phase 1 — V7 main path completion

These tasks advance the main V7 pipeline toward the master plan.

#### P1.1 — Stage D: XZ projection of ridge edges
- Project 3D ridge-edge-fit output onto XZ horizontal plane
- Each 3D plane-pair intersection line → 2D line segment
- **Prereq**: ridge-edge-fit (done)
- **Effort**: Small

#### P1.2 — Stage E: Horizontal edge graph processing
- Merge collinear edges (angle + offset tolerance)
- Split at intersections (T-junctions, crossings)
- Snap near-touching endpoints (gap < threshold)
- Output: clean 2D planar graph
- **Prereq**: P1.1
- **Effort**: Medium

#### P1.3 — Stage F: Footprint association — DP topology matcher
- Build cost matrix (detected edges × footprint edges): angular compatibility, perpendicular distance, length overlap
- DP alignment walking footprint edge sequence, transition costs enforce corner consistency
- Skip penalty for unmatched footprint edges
- Output: per-footprint-edge assignment
- **Prereq**: P1.2 + boundary-edge-detect (exists)
- **Effort**: Medium-Large

#### P1.4 — Stage F: Polygon reconstruction from matched edges
- Replace matched footprint edges with detected edge lines
- Intersect consecutive edge lines for corner vertices
- Unmatched edges keep BDTOPO position
- Validate (simple polygon, area ratio)
- **Prereq**: P1.3
- **Effort**: Medium

#### P1.5 — Stage G: Adapt categorize-planes for tile-wide + real footprint
- Current categorize-planes assumes building-scoped `owner` and BDTOPO footprint
- Adapt to consume tile-wide `owner` + reconstructed footprint from Stage F
- Separate roof planes (inside footprint) from non-building planes
- **Prereq**: P1.4
- **Effort**: Medium

#### P1.6 — Stage G: Wall plane detection
- Search for sparse vertical co-planar point clusters near eave edges
- Exploit multi-return wall returns (detect-boundary bit 6)
- Flag edge types: ridge, valley, eave, hip, dormer, wall-top
- **Prereq**: P1.5
- **Effort**: Medium-Large

#### P1.7 — density-weight boundary-edge-detect
- Current Hough votes are unweighted (each candidate = 1 vote)
- Weight by `point-weights` to prevent dense zones from dominating peaks
- Critical for tile-wide detection where density varies across buildings
- **Prereq**: point-weights already available from tile-signals
- **Effort**: Small

### Phase 2 — Dead-end branch migration from exploration

These tasks bring exploration's comparison branches into V7 for visual comparison in the UI. Each is independent and can be parallelized.

#### P2.1 — Tile cascade KDE dead-end
- Wire `normal-cascade-segment-kde → cascade-ridge-detect → normal-cascade-seed → competitive-grow → ransac-seed → competitive-grow → point-stealing` as dead-end branch after tile-signals
- Uses tile-signals' `building-points`, `point-normals`, `_kdTree` — already compatible
- **Not density-aware** — accept as-is for comparison (density bias is visible in the visualization, which is the point)
- **Effort**: Small (wiring only, steps exist)

#### P2.2 — Co-registration dead-end trio
- Wire `co-register`, `co-register-directional`, `co-register-dsm` as 3 parallel dead-end branches after `detect-boundary`
- Each reads `boundary-candidate-infos` and `footprint` — density-neutral (geometric matching)
- Keep `snap-footprint` as 4th dead-end after co-register for historical comparison
- **Effort**: Small (wiring only)

#### P2.3 — Building-scoped V6g chain dead-end
- Wire `scope-to-building → extract-denoise → compute-normals → normal-cascade-segment-kde → cascade-ridge-detect → normal-cascade-seed → competitive-grow → ransac-seed → competitive-grow → point-stealing`
- Requires scope transition (tile → building). Uses co-registered or raw footprint.
- **Not density-aware** — accept as-is for comparison
- **Effort**: Medium (scope transition wiring, instance numbering)

#### P2.4 — V3 categorization dead-end
- Wire `categorize-planes → asymmetric-steal` reading from the building cascade chain
- **Effort**: Small (wiring only)

#### P2.5 — Progressive cascade pass 2 dead-end
- Wire `normal-cascade-segment-kde → cascade-ridge-detect → normal-cascade-seed → competitive-grow` reading residuals from asymmetric-steal
- **Effort**: Small (wiring only)

#### P2.6 — V2 RANSAC baseline dead-end
- Wire `ransac-seed` reading from scoped building-points (after scope-to-building)
- **Effort**: Trivial

#### P2.7 — Axis 5 strip-consensus dead-end
- Wire `strip-region-grow → strip-consensus-merge` after scope-to-building
- Reads PSID metadata for flight strip separation
- Building-scoped, not density-aware — accept as-is
- **Effort**: Small (wiring only)

#### P2.8 — Boundary edge detection dead-end pair
- Wire `boundary-edge-detect` (Hough+RANSAC) and `boundary-edge-detect-knn-pca` as parallel dead-ends after detect-boundary
- Both read `boundary-candidate-infos` — compatible
- **Effort**: Small (wiring only)

### Phase 3 — 10-axis research integration

Each axis is an independent dead-end branch for visualization. Priority order matches research value.

#### P3.1 — Axis 9 (Edge-first boundary) dead-end
- Normal gradient analysis → boundary detection → flood fill between boundaries
- Reads `point-normals`, `point-weights` from tile-signals
- Tile-wide. Inherently boundary-based — minimal density concern.
- **Effort**: Medium (new step implementation from research notes)

#### P3.2 — Axis 8 (Noise-weighted RANSAC) dead-end
- Per-point σ from scan angle → weighted RANSAC → WLS refit
- Reads `point-normals` from tile-signals, scan angle from raw metadata
- Tile-wide. Complementary to point-weights (noise model vs density model).
- **Effort**: Medium (new step implementation)

#### P3.3 — Axis 1 (CT/Radon sweep) dead-end
- 810 Fibonacci hemisphere projections → 1D histogram peak detection
- Heavy computation. Pure geometry — no density concern.
- Tile-wide. Reads positions directly.
- **Effort**: Medium (new step implementation)

#### P3.4 — Axis 6 (Curvature-first classification) dead-end
- Principal curvatures → type classification → type-specific region growing
- Reads `curvatures-k16`, `planarity` from tile-signals — already available.
- Tile-wide.
- **Effort**: Medium (new step implementation)

#### P3.5 — Axes 2, 3, 4, 7 dead-ends
- Lower priority. Implement as time permits.
- All tile-wide, reading from tile-signals.
- **Effort**: Medium each

### Phase 4 — Advanced features

#### P4.1 — Stage H: Ortho-photo co-registration
- Extend `co-register-dsm` approach to use full 3D model edges
- Render synthetic edge image → cross-correlate with orthophoto Sobel edges
- `compute-ortho-artifacts` exists — adapt for V7
- **Effort**: Large

#### P4.2 — Stage I: Obstacle resolution + ortho confirmation
- Classify remaining unassigned points as obstacles
- Confirm boundaries via co-registered orthophoto
- Adapt `build-obstacle-records` for tile-wide input
- **Effort**: Large

#### P4.3 — Stage I: Texture mapping
- Project co-registered orthophoto onto 3D model faces as UV texture
- **Effort**: Large

#### P4.4 — Stage I: Solar panel fitting
- `fit-solar-panels` exists — works on plane hulls, density-neutral
- Wire into V7 after obstacle resolution
- **Effort**: Small (wiring only)

### Phase 5 — Density hardening

Retrofit `point-weights` into steps that are not yet density-aware. Only needed when they become production steps (not dead-ends).

#### P5.1 — Weight cascade-shared.js accumulators
- `histogram[bin] += weights[i]` instead of `+= 1` in Stage 1 and Stage 2
- `sumWeights(segment) >= minSegmentEffective` instead of `length >= minSegmentPts` in Stage 3
- Only matters if tile cascade KDE becomes a production path
- **Effort**: Medium (many touch points in cascade-shared.js)

#### P5.2 — Weight competitive-grow / point-stealing
- Currently uses unweighted distance comparisons
- Could weight distance thresholds by local noise (Axis 8 idea)
- **Effort**: Medium

#### P5.3 — Weight wall-roof-detect
- Already auto-tunes gap threshold from `lidar-density`
- Could use per-candidate weights for candidate scoring
- **Effort**: Small

---

## 10. Benchmark Baseline

**V7 initial** (Mar 12): 7/7 pass, avg 83.4, ~25s/case
**V6g-kde best** (reference): avg 90.9

Score gap reflects different segmentation strategy (tile-wide vs building-scoped) and absence of tuning. The tile-wide approach processes full tile points, hence higher latency.

**Expected trajectory:** Stages E–F (edge graph + footprint association) should recover and exceed V6 by providing geometrically accurate building outlines. Dead-end branches will reveal which approaches work best for which roof types, guiding future production path decisions.
