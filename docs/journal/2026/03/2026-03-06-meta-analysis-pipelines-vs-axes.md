---
id: "2026-03-06-meta-analysis-pipelines-vs-axes"
date: 2026-03-06
title: "Meta-Analysis: Existing Pipelines vs Research Axes — Philosophy, Math, and Combination Opportunities"
tags: [research, meta-analysis, synthesis, pipeline, combination, ensemble, comparison]
category: review
status: concluded
refs:
  - "2026-03-06-four-axis-research-synthesis"
  - "2026-03-06-axis1-radon-plane-detection"
  - "2026-03-06-axis2-multiview-rasterize-detect"
  - "2026-03-06-axis3-spectral-frequency-methods"
  - "2026-03-06-axis4-curvature-pca-region-growing"
  - "2026-02-26-v6-normal-cascade-implementation"
  - "2026-02-26-row-houses-v6-breakthrough"
  - "2026-02-26-cross-plan-structural-limitation"
  - "2026-02-26-dropped-algorithm-experiments"
---

# Meta-Analysis: Existing Pipelines vs Research Axes

## 0. Critical Preamble: Structural Biases in the Research Session

**Before interpreting any result in this document, the reader must understand two structural biases that shaped the research outcomes. These biases explain why four "independent" research axes, starting from four different mathematical traditions, converged to essentially the same algorithm.**

### 0.1 The Scorer Bias

All four agents were evaluated by a scorer that measures **planar segmentation quality** — plane matching, segmentation accuracy, boundary IoU. The ground truth for every test case (including silos) is a collection of flat planes. There is no curved surface GT, no parametric equation evaluation, no surface-type classification metric.

This means an agent that correctly detects a cylinder as a cylinder gets **zero reward**. The scorer asks "did you find the 12 faceted planes that approximate this cylinder?" — not "did you detect the cylinder?" Axis 4 built a curvature detection prototype (anti-normal ray convergence, stage 13 of 17), tested it, observed "convergence doesn't affect score since silo GT is plane-based," and moved on to planar post-processing tweaks that DO move the score.

**Every agent, regardless of starting thesis, was economically forced to optimize for planar segmentation.** Novel mathematical approaches that don't directly improve planar scores were systematically abandoned.

### 0.2 The Documentation Bias

Every agent was instructed to read, as prerequisites:
- `NORMAL-CASCADE.md` — which describes the 3-phase decomposition (orientation → offset → spatial CC)
- `STEP-CATALOG.md` — which lists 28 steps implementing that decomposition
- `PIPELINE-DEFINITIONS.md` — 22 pipelines all following the same pattern
- The research design doc itself (Section 6 "Mathematical Unification") — which pre-framed all 4 axes as "different computational paths to the same underlying geometry"

The agents absorbed 100+ lines of documentation describing orientation → offset → spatial CC as THE architecture before writing their first line of code. When an approach failed (Axis 3's graph Laplacian), the agent already knew exactly what to pivot to — because it had read the existing codebase.

### 0.3 The Convergence Evidence

| Axis | Original thesis | What the champion actually does | Thesis survival |
|------|----------------|-------------------------------|-----------------|
| 1 (CT/Radon) | Tomographic reconstruction from 800 projections | Normal histogram → offset histogram → spatial CC → RANSAC refinement | Fibonacci sweep is CT-inspired; the rest is standard decompose-then-assemble |
| 2 (Multi-view) | Project onto planes, detect in 2D raster images | Normal histogram → offset histogram → spatial CC → PCA refit | No actual multi-view geometry survived. It's V6 cascade with different parameters |
| 3 (Spectral) | Graph Laplacian Fiedler vector bisection | **Abandoned spectral entirely** → agglomerative normal clustering → offset histogram → spatial CC | Zero spectral math in the champion |
| 4 (Curvature) | PCA eigenvalue classification → curvature-guided processing | PCA classification preserved, but curvature detection is a 100-line prototype that doesn't affect scores. The 17-stage post-processing is pure engineering | Curvature thesis is vestigial; score gains come from planar refinement stages |

All four champions implement the same pattern: **estimate normals → group by orientation → separate by offset → spatial CC → iterative refinement.** The only differences are implementation details within each phase (histogram bins vs agglomerative clustering, fixed bins vs Otsu, grid CC vs k-NN CC).

### 0.4 What This Means for This Analysis

The combination opportunities identified below (Section 5) are real and valuable — k=20 normals, Otsu post-split, steal-back refinement, majority vote smoothing. These are genuine improvements discovered through intensive iteration. But they are **optimizations within a fixed architecture**, not novel algorithmic insights.

The research session validated that the 3-phase decomposition is optimal for **planar segmentation on plane-only GT**. Whether it's optimal for the broader problem (including curved surfaces, parametric fitting, and geometric completeness) remains untested — because the test cases and scorer don't measure those things.

**Action taken:** Curved surface test cases with parametric GT (cylinder, sphere, cone, ogive) are being implemented. These will create problems where the 3-phase decomposition cannot succeed (every point on a cylinder has a different normal — orientation grouping collapses). Future research agents given these test cases, a curvature-aware scorer, and NO access to existing pipeline documentation would be forced to derive genuinely novel algorithms.

---

## 1. Executive Summary

This document cross-analyzes the 22 existing bench pipelines (V2 through V6x) against the 4 research axes explored on 2026-03-06, comparing their mathematical foundations, philosophical approaches, per-case strengths/weaknesses, and — most importantly — identifying concrete combination opportunities where one method's strength compensates another's weakness.

### Key Finding

**The existing pipelines and the research axes are solving the same problem from fundamentally different mathematical starting points, but all converge on the same 3-phase decomposition pattern.** The differences lie in *how* each phase is implemented, and these implementation differences create exploitable complementarity.

### Score Overview (GT-scored, comparable test sets)

| System | Test Set | Avg Composite | Detection | SegF1 | BoundaryIoU |
|--------|----------|---------------|-----------|-------|-------------|
| **v6x-hybrid** (existing best) | 53 synth | 94.7 | 1.000 | 0.985 | 0.980 |
| **Axis 4** (research best) | 159 synth | 96.4 (research scorer) | — | — | — |
| **Axis 2** (research) | 159 synth | 95.1 (research scorer) | — | — | — |
| **Axis 3** (research) | 25 quick | 95.1 (research scorer) | — | — | — |
| **Axis 1** (research) | 159 synth | 88.7 (research scorer) | — | — | — |

**CAVEAT:** Direct numerical comparison is not possible — the scorers differ (bench composite_v2 with boundary IoU + overshoot vs research composite with segAcc + matchRate). The analysis below focuses on *which cases each method handles well or poorly* rather than absolute numbers.

---

## 2. Philosophical Comparison

### 2.1 The Decomposition Problem

All methods must answer three questions about each point:
1. **What orientation is this surface?** (normal estimation)
2. **Which specific plane at that orientation?** (offset/height separation)
3. **Is it spatially contiguous with its neighbors?** (connected components)

### 2.2 How Each System Answers

| Phase | V2 Champion | V6 Cascade | Axis 1 (Radon) | Axis 2 (Multi-view) | Axis 3 (Spectral) | Axis 4 (PCA+RG) |
|-------|-------------|------------|-----------------|--------------------|--------------------|------------------|
| **Normal** | Implicit (RANSAC samples random triples) | Explicit (2D tilt/az histogram on PCA normals) | Implicit (Fibonacci hemisphere sweep) | Explicit (20x20 hemisphere normal histogram) | Explicit (agglomerative clustering on unit sphere) | Explicit (k=20 PCA eigenvalue classification) |
| **Offset** | Implicit (RANSAC inlier distance) | Explicit (1D histogram of dot(p,n)) | Implicit (distance histogram per direction) | Explicit (1D offset histogram, bin=0.08m, valley detection) | Explicit (1D offset histogram, bin=0.3m) | Implicit (adaptive distance threshold during growth) |
| **Spatial** | CC splitting (k-NN flood fill) | XZ grid clustering | CC splitting (grid flood fill, 0.45m) | 2D raster CC (adaptive cell size) | Grid CC | CC splitting (k=20 k-NN) + multiple post-split stages |
| **Boundary** | Point stealing (distance ratio) | Point stealing + grow | RANSAC refinement (3 rounds) | PCA double refit + inlier filter | Iterative refinement (3 rounds) | Steal-back + 16-stage post-processing |

### 2.3 Mathematical Traditions

| System | Tradition | Core Operation | Determinism |
|--------|-----------|----------------|-------------|
| **V2** | Computational geometry | RANSAC random sampling → least squares | Stochastic (seed=42 for reproducibility) |
| **V3** | Signal processing | 1D elevation histogram → peak finding | Deterministic |
| **V6 Cascade** | Signal processing (2D+1D histograms) | Tilt/az binning → offset projection → spatial CC | Deterministic |
| **V6x Hybrid** | Signal processing + computational geometry | Cascade orientation + offset → per-segment RANSAC + CC | Mixed (cascade deterministic, RANSAC stochastic) |
| **Axis 1** | Tomographic reconstruction | Fibonacci hemisphere sweep → distance histograms → peak detection | Deterministic |
| **Axis 2** | Computer vision (multi-view) | Normal histogram → offset histogram → 2D raster projection CC | Deterministic |
| **Axis 3** | Spectral/statistical | Agglomerative normal clustering → offset histogram → spatial CC | Deterministic |
| **Axis 4** | Differential geometry + region growing | PCA eigenvalues → planarity-sorted growth → periodic refit | Deterministic |

### 2.4 Philosophical Taxonomy

Three fundamentally different philosophies:

**A. "Top-down global" (V2, Axis 1):** Start from the full point cloud, try to find global plane hypotheses (RANSAC random samples / Fibonacci sweep), then carve the cloud into pieces. *Strength:* robust to local noise. *Weakness:* combinatorial explosion with many planes, miss small surfaces.

**B. "Decompose-then-assemble" (V6, Axis 2, Axis 3):** Decompose the point cloud by orientation first, then by offset, then spatially. Each decomposition level narrows the search space. *Strength:* deterministic, handles many parallel planes. *Weakness:* fixed bin sizes can over/under-segment, ridge points get misassigned at decomposition boundaries.

**C. "Bottom-up local" (Axis 4):** Start from individual points, classify by local geometry (PCA curvature), then grow regions outward from the most planar seeds. *Strength:* adapts to local geometry, handles curved boundaries well. *Weakness:* growth order affects outcome, ridge contamination from noisy neighbors.

---

## 3. Mathematical Deep-Dive: Key Operations Compared

### 3.1 Normal Estimation

| Method | Formula | k | Robustness | Speed |
|--------|---------|---|------------|-------|
| V6 compute-normals | PCA on k-NN, flip to upward | 15 | Moderate (k=15 → 0.4m² influence) | O(n log n) |
| Axis 1 | None (implicit from sweep direction matching) | — | Low (no local normal) | O(n × 800) |
| Axis 2 | PCA on k-NN, grid spatial index | 15 | Moderate | O(n log n) |
| Axis 3 | PCA on k-NN, stride-based subsampling | 15 | Lower (subsampled) | O(n/s × k) |
| Axis 4 | PCA on k-NN, eigenvalue ratios | **20** | **Highest** (larger neighborhood) | O(n log n) |

**Key insight:** Axis 4's k=20 gives +2 composite over k=15 (documented in their v0.1→v0.3 progression). This is a **free improvement** for any pipeline that uses PCA normals — just increase k.

### 3.2 Orientation Grouping

| Method | Approach | Resolution | Edge Handling |
|--------|----------|------------|---------------|
| V6 histogram | 2D tilt(5°) × az(10°) grid, 3×3 smoothed peak detection | Fixed 5°/10° | Ridge candidates flagged (bit 0) |
| Axis 1 sweep | 800 Fibonacci directions, project all points, find distance peaks | ~3° angular resolution | No explicit edge handling |
| Axis 2 histogram | 20×20 hemisphere bins, greedy peak merging at 8° | Effective ~9° | None (downstream CC handles it) |
| Axis 3 clustering | Agglomerative on unit sphere, 20° angle threshold | Adaptive | None |
| Axis 4 PCA | No explicit grouping — eigenvalue ratios classify planarity | Point-level | Collinear normal inheritance (l2/l1 < 0.05) |

**Key insight:** Axis 2's 8° merge threshold is tuned to separate row house units (which differ by ~7° in normal). The V6 cascade uses 5° tilt bins + peak tolerance, which can also catch this — but row houses remain a weakness for both because the spatial CC phase, not the normal phase, must do the actual separation.

**Key insight:** Axis 4's collinear normal inheritance (points with l2/l1 < 0.05 inherit normal from nearest large neighbor) is a technique no existing pipeline has. It specifically fixes parapets (+15.1 on flat_with_high_parapet). This could be added as a preprocessing step before V6 cascade.

### 3.3 Offset Separation

This is the most diverse phase across methods:

| Method | Technique | Bin/Bandwidth | Separation Criterion |
|--------|-----------|---------------|---------------------|
| V6 histogram | 1D histogram of dot(p, group_mean_normal) | 0.15m fixed (or RMS-adaptive) | Peak finding + tolerance band |
| V6 KDE variants | Gaussian KDE (Silverman/IQR/physical/adaptive) | Auto bandwidth | Valley detection or posterior probability |
| Axis 1 | 1D distance histogram per sweep direction | 0.08m | Peak finding (count × sharpness scoring) |
| Axis 2 | 1D offset histogram per normal peak | **0.08m** (finer) + valley detection | Valley detection (explicit gap search) |
| Axis 3 | 1D offset histogram per normal cluster | **0.3m** (coarser) | Peak finding |
| Axis 4 | Otsu bimodal threshold on offsets | Adaptive (Otsu's method) | Bimodal threshold |

**Key insight:** Axis 2's 0.08m bin size with valley detection outperforms Axis 3's 0.3m bins. The V6 cascade's 0.15m bin is in between. Axis 2's approach is essentially what V6 does but with finer resolution — this is worth testing as a V6 parameter change.

**Key insight:** Axis 4's Otsu threshold is a fundamentally different approach — it doesn't need a bin size parameter at all. It's the optimal threshold for bimodal distributions. This is mathematically elegant but only works for exactly 2 offset groups. The histogram/KDE approach handles arbitrary numbers of parallel planes.

**Key combination opportunity:** Use Axis 4's Otsu as a *post-processing* validation step: after V6 cascade separates offset groups, verify that each group is unimodal (Otsu would not find a split). If Otsu finds a split within a V6 group, re-split. This catches cases where V6's bin size is too coarse.

### 3.4 Spatial Clustering

| Method | Approach | Cell/Radius | Min Size |
|--------|----------|-------------|----------|
| V6 cascade | XZ grid flood fill (1.0m cells) | 1.0m | 10-15 pts |
| V6x hybrid | Per-segment RANSAC + CC | k=15, 1.0m | 15 pts |
| Axis 1 | Grid flood fill (0.45m cells) | 0.45m | Implicit |
| Axis 2 | 2D raster CC (adaptive cell = max(0.1, 1.5×avgSpacing)) | **Adaptive** | Implicit |
| Axis 3 | Grid CC | Fixed | 5-8 pts |
| Axis 4 | k-NN CC (k=20) + post-split (k=5 for small surfaces with RMS guard) | k=20 / k=5 | **4 pts** |

**Key insight:** Axis 4's two-tier CC (k=20 for main split, k=5 for small surfaces with RMS≥0.05 guard) is a novel approach. The RMS guard prevents over-splitting clean small planes. This addresses the flat_with_antennas category where V6 and Axis 2 both struggle. Axis 4: 94.1 avg vs Axis 2: 78.6 avg on this category.

**Key insight:** Axis 2's adaptive cell size (based on average point spacing) self-calibrates to point density. V6's fixed 1.0m cell works for IGN LiDAR HD but may fail on sparser/denser data. Axis 2's approach is more robust to density variation.

---

## 4. Per-Case Complementarity Analysis

### 4.1 Category-Level Comparison (3 axes × 159 cases, research scorer)

Categories where methods strongly disagree (spread > 5 pts):

| Category | Axis 1 | Axis 2 | Axis 4 | Best | Spread | Why |
|----------|--------|--------|--------|------|--------|-----|
| proc-cathedral | 67.2 | **96.3** | 96.5 | A4 | 29.3 | A1's histogram can't resolve small peaked planes |
| proc-silo_with_barn | 65.9 | 95.4 | **96.4** | A4 | 30.5 | A1 can't represent cylinders as histogram peaks |
| proc-dormer_house | 72.0 | **97.2** | 97.6 | A4 | 25.6 | Tiny dormer surfaces below A1's resolution |
| proc-cross_plan | 78.1 | 77.7 | **96.6** | A4 | 18.5 | A4's region growing handles coplanar patches; A2's raster CC merges them |
| flat_with_antennas | 72.9 | 78.6 | **94.1** | A4 | 21.2 | A4's spatial re-split catches antenna clusters |
| flat_commercial_obstacles | 76.4 | 85.2 | **89.4** | A4 | 13.0 | Obstacle detection via PCA curvature classification |
| gable_with_parapet | 87.4 | **93.8** | 86.9 | A2 | 6.9 | A4's PCA through mixed parapet+slope gives tilted normals |
| mansard | 90.4 | **96.6** | 95.9 | A2 | 6.2 | A2's raster view cleanly separates mansard facets |
| proc-church | 74.4 | **97.3** | 96.7 | A2 | 22.9 | Multi-facet roofs need fine angular resolution |
| proc-octagonal_tower | 85.4 | **95.2** | 94.5 | A2 | 9.8 | 8 facets need fine normal resolution |
| proc-courtyard | 81.0 | **96.9** | 96.7 | A2 | 15.9 | Enclosed spaces need raster CC |

### 4.2 Oracle Ensemble Analysis

If we could pick the best of 3 axes per case (oracle selection):

- **Oracle: 91.25** (research scorer avg over 159 cases)
- **Axis 4 alone: 90.90** → Oracle gain: **+0.35** (modest)
- Oracle wins: A4=105, A2=42, A1=12 (of 159 cases)

The oracle gain is small because A4 dominates most cases. But the 54 cases where A4 is not best include some with large gains:

| Case | A4 Score | Oracle Score | Gain | From |
|------|----------|-------------|------|------|
| gable_with_parapet-default | 82.6 | 96.2 | +13.6 | Axis 2 |
| gable_with_parapet-small | 80.9 | 87.5 | +6.6 | Axis 2 |
| flat_commercial_obstacles-small | 89.4 | 93.5 | +4.1 | Axis 2 |
| mansard-small | 91.9 | 95.3 | +3.4 | Axis 2 |
| proc-split_level-large | 90.8 | 93.6 | +2.8 | Axis 1 |
| proc-clerestory-medium | 85.7 | 88.1 | +2.4 | Axis 2 |

**The pattern is clear:** Axis 2 (multi-view raster) beats Axis 4 specifically on:
- **Parapets** (gable_with_parapet, flat_large_parapet) — A4's PCA gets contaminated by mixed parapet+slope points
- **Mansard roofs** — A2's raster projection cleanly separates the 4 mansard facets
- **Many-faceted buildings** (church, octagonal_tower, cathedral) — A2's fine normal histogram resolves more orientations

### 4.3 Existing Pipeline Weakness Patterns

From the bench DB, the top pipelines (v6x-hybrid, v6f-minimal) show consistent weaknesses:

| Weakness | Pipelines Affected | Avg Composite | Root Cause |
|----------|-------------------|---------------|------------|
| **multi_section** | All V6 | 90-93 | Boundary IoU loss — adjacent planes share ridge points |
| **mansard** | All V6 | 93-94 | 4 tightly-packed facets, SegF1 drops to 0.94-0.95 |
| **asymmetric_gable** | v6f-minimal | 93.1 | One case with SegF1=0.95 (misassigned ridge line) |

Notably, all existing pipelines score 93+ on their worst cases — the floor is high. But the V6 pipelines are scored on only 7-9 distinct building types (clean synth), while the research axes were scored on 50+ types including proc-* buildings.

---

## 5. Concrete Combination Opportunities

### 5.1 Quick Wins (low effort, high confidence)

#### A. **k=20 normals for V6 cascade** (from Axis 4)
- **What:** Change `normalK: 15` to `normalK: 20` in all V6 pipeline defaults
- **Why:** Axis 4 documented +2 composite gain from k=15→k=20. Larger neighborhood = more robust PCA normal
- **Risk:** Slightly slower (more k-NN queries). May blur ridges on very small buildings.
- **Test plan:** Param-grid `--param-grid normalK=[15,20,25]` on synth test set

#### B. **Collinear normal inheritance** (from Axis 4)
- **What:** New preprocessing step after `compute-normals`. For points with eigenvalue ratio l2/l1 < 0.05 (linear, not planar), replace their normal with the normal of the nearest large-neighborhood point.
- **Why:** Parapets have near-collinear point distributions. PCA on collinear points gives random normals that poison downstream histogram binning. Inheriting from a neighboring planar surface fixes this.
- **Impact:** +15.1 on flat_with_high_parapet (Axis 4's result). Could fix gable_with_parapet for existing pipelines.
- **Risk:** Low. Only affects the ~2% of points that are truly collinear. No impact on non-parapet cases.
- **Implementation:** Add to `compute-normals` step or create new `inherit-normals` step.

#### C. **Finer offset histogram bin size** (from Axis 2)
- **What:** Reduce `DEFAULT_OFFSET_BIN_SIZE` from 0.15m to 0.08m in cascade-shared.js
- **Why:** Axis 2 uses 0.08m and achieves better separation of closely-spaced parallel planes. The V6 KDE variants tried to address this with continuous density estimation but regressed (all v6g-kde-* pipelines scored 41-55, far below v6a's 43).
- **Risk:** More bins = more fragmentation on noisy data. Need to pair with adjusted peak merging.
- **Test plan:** Param-grid offset bin sizes [0.06, 0.08, 0.10, 0.12, 0.15, 0.20]

### 5.2 Medium-Effort Combinations

#### D. **Otsu post-split validation** (from Axis 4)
- **What:** After V6 cascade offset split, apply Otsu bimodal test to each resulting group. If Otsu finds a significant split (valley ratio > threshold), re-split the group.
- **Why:** Catches under-segmented offset groups that the fixed-bin histogram missed.
- **Impact:** Specifically targets flat_multi_level and flat_commercial_obstacles cases.
- **Implementation:** Add 30-line Otsu function to cascade-shared.js, call after stage 2.

#### E. **Axis 2's adaptive raster CC as spatial stage** (from Axis 2)
- **What:** Replace V6 cascade's XZ grid flood fill (1.0m cells) with Axis 2's adaptive cell-size raster CC.
- **Why:** V6's fixed 1.0m cell size assumes IGN LiDAR HD density. Axis 2's `max(0.1, 1.5×avgSpacing)` self-calibrates.
- **Impact:** More robust on variable-density scans, slightly better on small surfaces.
- **Implementation:** Extract Axis 2's raster CC function, integrate as cascade stage 3 alternative.

#### F. **PCA curvature pre-classification** (from Axis 4)
- **What:** Before the V6 cascade, classify each point by its PCA eigenvalue ratios: planar (planarity > 0.5), linear (linearity > 0.5), spherical (rest). Use classifications to weight or exclude from histogram peaks.
- **Why:** Ridge points (linear) contaminate normal histogram peaks. Spherical points (noise) add false counts. Excluding or down-weighting them sharpens peaks.
- **Impact:** Could improve multi_section and mansard (where ridges are the bottleneck).
- **Implementation:** Add to `compute-normals` step, emit `point-curvature` artifact.

#### G. **Steal-back boundary refinement** (from Axis 4)
- **What:** After point-stealing, do a reverse pass: for each point, check if its current plane's boundary points would fit better in the neighboring plane. Swap back if the neighboring plane gives better RMS.
- **Why:** Standard point stealing is greedy (one direction). Steal-back allows boundaries to settle at the true minimum.
- **Impact:** +0.2 avg, +1.7 on gambrel (Axis 4's documented result).
- **Implementation:** Modify `point-stealing.js` to add reverse pass.

### 5.3 High-Effort Combinations (New Pipeline)

#### H. **Hybrid ensemble: V6x + Axis 4 post-processing**
- **What:** Run V6x cascade segmentation + RANSAC seeding (existing pipeline), then apply Axis 4's 16-stage post-processing pipeline: size-aware merge, offset histogram split (Otsu), height outlier split (MAD), silo protection, residual recovery, mini-plane recovery, anti-normal convergence, spatial re-split, selective micro-fragment absorption.
- **Why:** V6x has the best segmentation front-end (normal cascade + per-segment RANSAC). Axis 4 has the most sophisticated post-processing (16 stages, 42 versions of iteration). Combining them gets the best of both.
- **Estimated impact:** V6x's 94.7 + Axis 4's post-processing improvements could reach 96-97 on the bench test set.
- **Risk:** The post-processing stages are tuned for Axis 4's specific segmentation output. They may need re-tuning for V6x output.
- **Implementation:** Port Axis 4's stages 5-16 into new pipeline steps.

#### I. **Per-building-type router** (meta-ensemble)
- **What:** Classify the building type (flat, gabled, mansard, complex) from the point cloud statistics (height range, normal distribution entropy, footprint shape), then route to the best pipeline for that type.
- **Why:** No single pipeline wins on all types. Axis 4 wins 66% of cases, Axis 2 wins 26%, existing pipelines have different sweet spots. A classifier that routes to the right method per building captures most of the oracle gain.
- **Routing table:**
  - Flat roofs (low tilt variance) → histogram-seed + V3 path
  - Gable/hip (2 dominant orientations) → V6x hybrid
  - Mansard/church (4+ orientations) → Axis 2 multi-view
  - Complex with parapets → Axis 4 PCA + region growing
  - Complex with obstacles → Axis 4 with spatial re-split
- **Estimated impact:** ~0.5 composite gain (oracle analysis shows +0.35 from best-of-3-axes; adding existing pipeline options as candidates could reach +0.5).
- **Implementation:** Building classifier + pipeline router step.

#### J. **Iterative cascade refinement** (from Axis 3 + Axis 4)
- **What:** After initial segmentation (V6 cascade or Axis 2), run 3 rounds of: assign all points to nearest plane → refit plane → repeat.
- **Why:** Axis 3 documented this as their "single most impactful technique" — without it, segAcc is 0.14-0.53; with 3 rounds it reaches 0.92-1.0. Axis 4's periodic PCA re-fit during growth achieves the same effect.
- **How it relates to existing:** The existing V6 pipeline does `competitive-grow → point-stealing → refit-planes`, which is already 3 stages of refinement. But the refinement is local (k-NN neighborhoods). Global refit (reassign ALL points to their closest plane, refit ALL planes) may catch points that local growing missed.
- **Risk:** Could break spatial contiguity (a point assigned to a distant plane it's geometrically close to but not spatially connected to).
- **Mitigation:** Add connectivity check to the reassignment step.

---

## 6. The V6G KDE Failure — Lessons for Integration

The V6G KDE experiments (5 Stage 2 variants + 3 Stage 1 variants) all scored 41-55 composite, dramatically below the histogram-based V6 pipelines (94+). This is the single largest research investment that failed in the existing bench.

### Why KDE failed

1. **Bandwidth sensitivity:** Silverman bandwidth is designed for unimodal distributions. Roof offset distributions are multimodal (multiple parallel planes). Silverman over-smooths, merging distinct peaks.
2. **Valley detection fragility:** KDE valleys are sensitive to kernel shape. A single noisy point near a valley can shift it, splitting a continuous plane.
3. **Scale mismatch:** The 0.03m sensor noise creates KDE bandwidth of ~0.05m, but meaningful plane separations are 0.1-3.0m. The KDE is operating at the wrong scale.

### Comparison with Axis 2

Axis 2 also uses offset histograms (bin=0.08m) but with simple valley detection — and it works (95.1 composite). The difference:
- Axis 2 uses **fixed-width bins** (not kernel density), which don't suffer from bandwidth sensitivity
- Axis 2 uses **valley detection** (drop between peaks), not posterior probability assignment
- Axis 2's bin size (0.08m) is between sensor noise (0.03m) and typical plane separation (0.1m+) — the right scale

### Lesson

**Simple histograms with well-chosen bin sizes beat sophisticated KDE on this problem.** The offset distribution is not smooth enough for KDE to add value. The V6G research was a dead end, but it confirms that the histogram approach (V6 cascade, Axis 2) is the right tool.

---

## 7. Historical Context from Past Experiments

### 7.1 Dropped Experiments That Axes Vindicate

| Past Experiment | Why Dropped | Axis That Succeeded With Similar Idea |
|----------------|-------------|---------------------------------------|
| Ridge-first paradigm (Hessian + flood fill) | Catastrophic — boundaries don't align with ridges | **Axis 4** succeeded with the opposite: grow FROM planes, not along ridges |
| Hull dilation + clip | Less precise than exact corner computation | Irrelevant — all axes avoid hull-based approaches |
| DSM gradient clustering | 15pts worse (80.3 composite) | **Axis 2** uses DSM-like raster views but projects along the plane normal, not vertically |
| Normal histogram seeding V4 | Tied with V2, multi_section weakness fundamental | **V6 cascade** fixed this by adding offset stage + spatial CC |

### 7.2 Progressive Enhancement History

The evolution from V2 → V3 → V4 → V6 → V6x mirrors the research axes' convergent discovery:

```
V2 (RANSAC only)           ← pure top-down global
  ↓ add histogram for flat roofs
V3 (histogram + RANSAC)    ← flat-roof optimization
  ↓ add 2D normal histogram
V4 (normal histogram)      ← orientation-aware seeding
  ↓ add offset histogram + spatial CC + ridge detection
V6 (normal cascade)        ← 3-stage decompose-then-assemble
  ↓ replace global spatial CC with per-segment RANSAC
V6x (hybrid cascade)       ← best of cascade + RANSAC
```

The research axes, working in parallel and independently, arrived at the same architecture:
- Axis 2 ≈ V6 cascade (normal histogram → offset histogram → spatial CC) with different parameters
- Axis 4 ≈ V4 + extensive post-processing (PCA normals → region growing → 16-stage refinement)
- Axis 3 ≈ V4 with agglomerative clustering instead of histogram binning
- Axis 1 ≈ V2 RANSAC but with exhaustive direction sweep instead of random sampling

**This convergence is strong evidence that the 3-phase decomposition (orientation → offset → spatial) is the correct architecture.** Future improvements should focus on better implementation of each phase, not alternative architectures.

---

## 8. The Curvature Gap — What No One Has Solved

### 8.1 Current State: Zero Curvature Detection in Production

Our entire pipeline stack (V2 through V6x) treats every surface as planar. There is no cylinder fitting, no sphere detection, no curvature estimation, no dome/silo modeling. When a building has a curved roof (grain silo, turret, dome), the pipeline approximates it as a collection of narrow planar facets — and this happens to work reasonably well because the scorer also uses plane-based GT (silos are scored as 10-12 faceted planes).

This means **our pipeline accidentally does OK on curved surfaces by over-segmenting them into planar strips**, but it has no geometric understanding of curvature. This matters for:
- **Downstream applications** (solar analysis, rainwater flow, structural modeling) that need true surface geometry
- **Point efficiency** — a cylinder described by 1 equation vs 12 plane equations
- **Robustness** — tight planar facets on curves are fragile (noise pushes points between adjacent facets)

### 8.2 What the Research Axes Found

| Axis | Curvature Approach | Result |
|------|-------------------|--------|
| **Axis 4** | Anti-normal ray convergence (`detectCurvedSurfaces()` — 100-line prototype) | Correctly detects cylinders when normals are consistently outward. **Fails on spheres** (y>0 normal convention breaks orientation). Score unchanged because GT is plane-based |
| **Axis 2** | None — purely planar model | — |
| **Axis 3** | None — normal clustering inherently assumes planar groups | — |
| **Axis 1** | None — distance histogram peaks only form for planar concentrations, not curved | — |

**Only Axis 4 attempted curvature detection**, and even there it's a prototype (stage 13 of 17) that only runs on residual points after all planar extraction is complete. The approach:
1. Cluster residual points by spatial proximity (k-NN)
2. For each cluster, compute normal convergence: extend each point's normal as a ray, find nearest approach to other rays
3. If rays converge to a line → cylinder. If to a point → sphere.
4. Classify by comparing convergence spread vs cluster spread

**Key finding from Axis 4's deep-dive (test-convergence.js):**
- Plane: correctly classified (convergence points scatter far away)
- Cylinder: correctly detected when normals are consistently outward
- Sphere: **FAILS** with y>0 convention — normals are inconsistently oriented for non-planar surfaces
- Centroid-based re-orientation helps but doesn't fully solve (centroid can be inside surface)

### 8.3 Silo Scores — The Accidental Success

All axes score well on silos because they over-segment the cylinder into planar facets that match the plane-based GT:

| Case | Axis 1 | Axis 2 | Axis 4 | Notes |
|------|--------|--------|--------|-------|
| proc-silo-large | 81.6 | 94.4 | **96.3** | A1 detects 15 planes (GT=12), poor segAcc |
| proc-silo-medium | 92.9 | 96.3 | **97.2** | All axes match plane count |
| proc-silo-small | 83.6 | **90.7** | 90.6 | Low point count hurts all |
| proc-silo_with_barn-large | 68.6 | 96.8 | **97.3** | A1 only finds 3 planes (GT=14) — barn occluded |
| proc-silo_with_barn-medium | 67.5 | 96.2 | **96.5** | Same A1 failure |
| proc-silo_with_barn-small | 61.5 | 93.3 | **96.1** | A1 collapses completely |

Axis 4's silo protection (curvature-variation-aware absorption tightening, v0.14) prevents its fragment absorption from merging silo facets into one. Without this protection, the silo's thin planar strips get absorbed into neighbors, destroying segmentation.

### 8.4 What's Needed

True curvature detection requires:
1. **Reliable normal orientation** for non-convex surfaces (the fundamental blocker — Axis 4 documented this)
2. **Primitive fitting** beyond planes: RANSAC for cylinder (5 params), sphere (4 params), cone (7 params)
3. **A scorer that understands curvature** — current GT and scorer are plane-only
4. **Surface-type classification**: before fitting, decide if a point cluster is planar, cylindrical, spherical, or conical (PCA eigenvalue ratios give planarity/linearity/sphericity, but distinguishing cylinder from cone needs more)

This is a research gap that none of the 4 axes closed. Axis 4's prototype is the closest starting point but needs the normal orientation problem solved first.

---

## 9. Recommended Next Steps (Priority Order)

### Tier 1 — Direct parameter changes (< 1 hour each)

1. **normalK=20** in all V6 pipelines (Axis 4's documented +2 gain)
2. **Offset bin size sweep** (0.06-0.20m) to find optimal for V6 cascade
3. **cascadeMinSeedPts=10** (v6f-minimal already uses this, others use 15)

### Tier 2 — New step implementations (1-3 hours each)

4. **Collinear normal inheritance** step (Axis 4's l2/l1 < 0.05 technique)
5. **Otsu post-split** after cascade stage 2
6. **Steal-back reverse pass** in point-stealing step
7. **Two-tier spatial CC** (k=20 main + k=5 for small surfaces with RMS guard)

### Tier 3 — Incremental new pipeline variants (3-8 hours each)

8. **v7-curvature-aware**: V6x + curvature pre-classification + collinear inheritance + steal-back
9. **v7-hybrid-postproc**: V6x front-end + Axis 4's post-processing stages
10. **v7-ensemble-router**: Building type classifier + pipeline router

### Tier 4 — Port & compare (enable apples-to-apples)

11. Port Axis 2 detector.js into bench pipeline format (would enable direct composite_v2 comparison)
12. Port Axis 4 detector.js into bench pipeline format
13. Run bench composite_v2 scoring on all axes for apples-to-apples comparison
14. Investigate whether Axis 4's periodic PCA re-fit during growth can replace the existing grow→steal→refit sequence

---

## 10. Cross-Domain Pipeline Architectures (Beyond Incremental)

The Tier 1-4 next steps above are all about **enriching the existing V6 pipeline** with cherry-picked techniques from the research axes. This is necessary but insufficient — it's optimizing within a fixed architecture. The research axes revealed opportunities for fundamentally different pipeline architectures that combine multiple analysis domains.

### 10.1 Multi-Resolution Cascade Pipeline

**Concept:** Run the same point cloud through 3 analysis scales in sequence, each catching what the previous missed.

```
Scale 1: Coarse orientation analysis (Axis 1-style hemisphere sweep, ~100 directions)
  → Identify dominant orientations + approximate plane count
  → Output: orientation priors + rough segmentation

Scale 2: Per-orientation fine analysis (Axis 2-style raster projection per detected orientation)
  → Project points in each orientation group onto 2D raster
  → Detect spatial clusters + offset separations with sub-centimeter resolution
  → Output: refined segments with accurate boundaries

Scale 3: Local boundary refinement (Axis 4-style PCA region growing on boundary zones only)
  → For each inter-segment boundary strip (±0.5m from boundary)
  → Re-grow from both sides with periodic PCA re-fit
  → Steal-back + majority vote smoothing
  → Output: final segmentation with clean boundaries
```

**Why this is different from V6x:** V6x runs cascade→RANSAC→grow sequentially on the full cloud. This architecture applies the *right method at the right scale*: global sweep for orientation discovery (Axis 1's strength), per-orientation raster analysis for fine separation (Axis 2's strength), local PCA growth for boundary precision (Axis 4's strength). Each scale uses a fundamentally different mathematical framework.

**Expected gain:** Scale 2 specifically addresses the offset separation problem that V6's histogram struggles with on multi-level buildings. Scale 3 addresses the boundary noise that costs 2-5 points on boundary IoU.

### 10.2 Dual-Path Consensus Pipeline

**Concept:** Run two fundamentally different segmentation approaches in parallel, then reconcile.

```
Path A: Decompose-then-assemble (V6 cascade or Axis 2)
  → Orientation → offset → spatial CC
  → Strength: deterministic, handles parallel planes, fast

Path B: Bottom-up region growing (Axis 4)
  → PCA classification → planarity-sorted growth → post-processing
  → Strength: adapts to local geometry, better boundaries

Consensus stage:
  → For each point, both paths propose a segment assignment
  → If both agree → high confidence, keep
  → If they disagree → "contested zone" (typically boundaries, ridge lines, parapets)
  → For contested zones: use the assignment from whichever path has lower local RMS
  → OR: re-fit planes locally using only the contested points + their confident neighbors
```

**Why this matters:** The oracle ensemble analysis (Section 4.2) shows +0.35 avg from best-of-3-axes. But oracle selection is per-building, not per-point. A consensus pipeline operates at the *point level* — it can use Path A's assignment for the flat center of a plane (where decompose-then-assemble excels) and Path B's assignment at boundaries (where region growing excels). The theoretical ceiling is higher than building-level oracle selection.

**Key challenge:** Runtime is 2x (both paths must complete). But Axis 2 and Axis 4 both run in <500ms per building — 2x is still fast enough for production.

### 10.3 Geometry-Typed Pipeline with Curvature Branch

**Concept:** Classify point regions by geometric type *first*, then route to specialized fitting.

```
Phase 0: PCA curvature classification (Axis 4's eigenvalue ratios)
  → Every point gets: planarity, linearity, sphericity scores
  → Cluster into regions by dominant geometry type

Phase 1a: Planar regions (planarity > 0.5)
  → Standard V6 cascade or Axis 4 region growing
  → Well-understood, current best methods apply

Phase 1b: Linear regions (linearity > 0.5)
  → Ridge/edge candidates → use for boundary confirmation, not surface fitting
  → Collinear normal inheritance for parapets
  → Edge-aware CC splitting

Phase 1c: Curved regions (sphericity > threshold OR planarity < 0.3 in connected clusters)
  → RANSAC for cylinder (5 params) / sphere (4 params) / cone (7 params)
  → Axis 4's anti-normal convergence as fast pre-filter
  → Need: robust normal orientation (propagated from planar neighbors)

Phase 2: Cross-type boundary reconciliation
  → Where planar meets curved: find the exact transition line
  → Where ridge meets plane: snap boundary to ridge
  → Output: hybrid geometric model (planes + primitives + edges)
```

**Why this is needed:** Current pipelines force everything through the "planar" path. A silo is described by 12 faceted planes. A turret dome is described by 8-20 micro-planes. This is geometrically wrong and fragile. The geometry-typed pipeline would output `{type: 'cylinder', axis: [...], radius: R}` for a silo — one equation instead of 12.

**Blocker:** The scorer must be extended to evaluate curved primitive fits, not just planar segmentation. This is both an algorithmic challenge and an infrastructure change to the bench harness.

### 10.4 Iterative Multi-Technique Refinement

**Concept:** Instead of a fixed pipeline, iterate between techniques until convergence.

```
Iteration 0: Axis 2 multi-view raster (fast, gets 95% of structure right)
  → Output: initial segmentation S0

Iteration 1: For each segment in S0, compute fit quality metrics
  → High RMS segments → re-analyze with Axis 4 region growing (local PCA re-fit)
  → Under-segmented segments (Otsu bimodal test) → re-split with finer offset histogram
  → Over-segmented fragments → attempt merge with Axis 3's agglomerative clustering
  → Output: improved segmentation S1

Iteration 2: Global consistency check
  → Axis 3's iterative refinement: assign ALL points to nearest plane, refit, repeat
  → Contested boundary smoothing with majority vote (Axis 4 v49)
  → Output: S2

Iterate until ΔcompositeScore < 0.1 (typically 2-3 iterations)
```

**Why this is powerful:** Each iteration uses the *right technique for the right problem*:
- Initial structure: fast raster approach (Axis 2)
- Bad fits: local re-analysis (Axis 4)
- Under-segmentation: statistical splitting (Axis 4 Otsu)
- Over-segmentation: agglomerative merge (Axis 3)
- Global cleanup: iterative refinement (Axis 3) + majority vote (Axis 4)

No single technique is asked to solve the whole problem. Each technique addresses the failure mode it's best at fixing.

**Estimated complexity:** 2-3 days. The individual techniques already exist in the research axis detectors; the challenge is orchestrating the iteration loop and convergence criteria.

### 10.5 Summary: Incremental vs Architectural

| Approach | Type | Expected Gain | Effort | Risk |
|----------|------|---------------|--------|------|
| Tier 1-2 parameter tweaks | Incremental | +1-3 composite | Hours | Low |
| Tier 3 v7 variants | Incremental | +2-4 composite | Days | Medium |
| Multi-resolution cascade (10.1) | Architectural | +3-5 composite | 2-3 days | Medium |
| Dual-path consensus (10.2) | Architectural | +2-4 composite, better boundaries | 2-3 days | Medium |
| Geometry-typed with curvature (10.3) | Architectural | Qualitative leap (curved surface support) | 1 week+ | High (scorer changes) |
| Iterative multi-technique (10.4) | Architectural | +3-6 composite | 2-3 days | Medium |

**Recommendation:** Start with 10.4 (iterative multi-technique refinement) — it's the most pragmatic architectural change because it reuses existing detector implementations and needs no scorer changes. The iteration loop is a natural extension of the current pipeline runner. Follow with 10.2 (dual-path consensus) as a complementary approach.

10.3 (geometry-typed with curvature) is the most scientifically interesting but requires scorer infrastructure work before results can be measured. Plan this for when curved-surface GT becomes available.

---

## 11. Summary Table

| Technique | Source | Applicable To | Expected Gain | Effort |
|-----------|--------|---------------|---------------|--------|
| k=20 normals | Axis 4 | All V6 | +1-2 composite | Trivial |
| Collinear inheritance | Axis 4 | All V6 | +2-15 on parapet cases | 1h |
| Finer offset bins (0.08m) | Axis 2 | V6 cascade | +0.5-1 on multi-level | 30min |
| Otsu post-split | Axis 4 | V6 cascade | +1-3 on under-segmented | 2h |
| Adaptive raster CC | Axis 2 | V6 cascade S3 | Robustness gain | 2h |
| PCA curvature classification | Axis 4 | V6 cascade S1 | +0.5-1 on ridge cases | 3h |
| Steal-back refinement | Axis 4 | All pipelines | +0.2 avg | 1h |
| Two-tier spatial CC | Axis 4 | V6 cascade S3 | +5-15 on antenna/obstacle | 2h |
| Axis 4 post-processing stages | Axis 4 | New V7 pipeline | +2-4 composite | 8h |
| Building-type router | Meta | New V7 ensemble | +0.5 avg | 8h |
| Multi-resolution cascade | Cross-domain | New architecture | +3-5 composite | 2-3 days |
| Dual-path consensus | Cross-domain | New architecture | +2-4 composite | 2-3 days |
| Geometry-typed + curvature | Cross-domain | New architecture | Qualitative leap | 1 week |
| Iterative multi-technique | Cross-domain | New architecture | +3-6 composite | 2-3 days |

---

## 12. Files

- Research axis results: `research/axis-{1,2,3,4}-*/results/`
- Research axis detectors: `research/axis-{1,2,3,4}-*/detector.js`
- Bench pipeline definitions: `lib/pipeline/pipelines.js`
- Bench results DB: `results/bench.db`
- V6 cascade implementation: `lib/pipeline/steps/cascade-shared.js`
- Normal cascade design doc: `docs/NORMAL-CASCADE.md`
- Research design doc (journal): `docs/journal/2026/03/2026-03-06-next-gen-pipeline-research-axes.md`
- Per-axis journal entries: `docs/journal/2026/03/2026-03-06-axis{1,2,3,4}-*.md`
- Synthesis journal entry: `docs/journal/2026/03/2026-03-06-four-axis-research-synthesis.md`
