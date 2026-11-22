---
id: "2026-03-15-ransac-density-compensation-revisited"
date: 2026-03-15
title: "ransac-full/poly: density compensation revisited — NaN bug invalidates prior conclusions"
tags: [ransac, density, bugfix, finding, v7]
category: finding
status: concluded
refs:
  - "2026-03-15-ransac-full-density-compensation"
  - "2026-03-15-ransac-poly-curved-surface-detection"
pipeline: v7
---

# ransac-full/poly: density compensation revisited

## Prior conclusions were wrong

The journal entries `2026-03-15-ransac-full-density-compensation` and
`2026-03-15-ransac-poly-curved-surface-detection` concluded that:

1. "Coplanar cross-roof mixing is a fundamental RANSAC limitation"
2. ransac-full plateaus at 12 planes / 66% on Dahouet (multi-building coastal tile)
3. Density compensation (weighted sampling, weighted scoring, weighted area gates) is the correct approach

**All three conclusions were drawn from corrupted experiments.** A point-weights
indexing bug caused all weights to be `NaN` during those experiments. Under NaN,
the code fell back to effectively random behavior — the density compensation
stack had no effect. The results attributed to "density compensation fixes"
were artifacts of the NaN fallback path.

## The bug: R2 migration gap

The R2 global index refactor (March 13, commits `a93d14c`→`daff588`) migrated
all per-point arrays to global indexing (length N). But `tile-signals` still
emitted `point-weights` as a cascade-filtered array (length M, parallel to
`building-indices`) when `tileExcludeClasses` was set.

The density compensation work (March 14-15, commits `c0291ec`→`c91bd7e`) added
`point-weights` as a dependency to 7 steps, indexing it with global indices
(`pointWeights[gi]`). When M ≠ N (e.g. exploration pipeline: M=4251, N=12907),
`pointWeights[gi]` for gi > M returns `undefined` → NaN.

**Impact of NaN**:
- `sumArea()` returns NaN → area gate `NaN < 0.5` is `false` → never stops
- `ransacPlaneWeighted` scoring uses NaN → falls back to first non-NaN result
- ransac-full entered infinite loops on non-planar surfaces (quonset test case)

**Fix**: `tile-signals` now always emits `point-weights` globally indexed
(length N), completing the R2 migration. One-line change in the producer.

## Re-evaluation with correct weights

With the NaN bug fixed, the true impact of weighted density compensation became
visible — and it was **harmful**, not helpful.

### Weight distribution on Dahouet (density 30.5 pts/m²)

| Stat | Value |
|------|-------|
| min weight | 0.010 |
| median weight | 0.026 |
| max weight | 4.655 |
| ratio max/median | **179×** |

Dense in-band points (majority): weight ~0.02. Sparse inter-band points: weight
~4.6. This 180× variance is the core problem.

### Weighted scoring biases toward sparse noise

`ransacPlaneWeighted` selects the plane with highest `Σ weights[inliers]`.
With 180× weight variance:
- 5 sparse points: score = 5 × 4.65 = **23.25**
- 500 dense points: score = 500 × 0.02 = **10.0**

RANSAC preferentially selects planes through sparse inter-band gaps over planes
through actual dense building surfaces.

### Weighted sampling over-represents sparse points

Sample probability ∝ weight. A single sparse point (w=4.65) has 180× the
sampling probability of a dense in-band point (w=0.026). Plane hypotheses are
dominated by samples from noisy gap regions.

### Weighted area gate is 36× too strict

To reach 0.5 m² at median weight: `0.5 × 30.5 / 0.026 = 588 points`.
Without weights: `0.5 × 30.5 = 16 points`.
A CC of 400 dense in-band points (clearly a legitimate roof face) has weighted
area 0.34 m² — rejected.

## The fix: selective density compensation

Density compensation helps in some RANSAC phases but actively hurts in others:

| Phase | Before (v5-v7) | After (v8) | Rationale |
|-------|---------------|------------|-----------|
| **Sampling** | Weighted | **Uniform** | Dense in-band points are reliable measurements. Sparse gap points are noisy. Uniform gives the good signal fair representation. |
| **Scoring** | Weighted sum | **Inlier count** | "Best plane" = most measurement agreement, not most physical area. Count finds the plane that the data most strongly supports. |
| **CC connectivity** | Pair-radius ✓ | **Pair-radius ✓** | Correct — sparse points genuinely need wider search to bridge scan-line gaps. |
| **Size gate** | Area only | **Dual: area OR count ≥ 15** | Weighted area is physically correct but fragile with 180× variance. Count gate catches dense surfaces that area alone rejects. |
| **LS refit** | Weighted ✓ | **Weighted ✓** | Correct — each point's influence on the fit should be proportional to the area it represents. |

## Results

### Dahouet (real-22186-dahouet-502)

| Version | Planes | Assigned | Notes |
|---------|--------|----------|-------|
| v5 (weighted, NaN bug) | 12 | 66% | Journal-reported. NaN weights = effectively uniform by accident. |
| v6 (weighted, bug fixed) | 9 | 61% | True weighted behavior revealed — worse. |
| v8 (uniform + count + dual gate) | **282** | **70%** | Dramatic improvement. |

### Synthetic quick set (exploration pipeline)

7/7 passed, avg 91.7 — identical to pre-change. No regression on synthetic
cases. ransac-full is a dead-end visualization branch; the main pipeline score
comes from the hemisphere cascade, which is unchanged.

## Additional fixes in this session

| Fix | Impact |
|-----|--------|
| Owner `Int8Array` → `Int16Array` via `OwnerArray` typedef | Overflow at 128+ planes caused grey/uncolored points in UI |
| Removed grow pass (no spatial constraint) | Was assigning points across buildings to coplanar planes 20m away |
| `connectDist` reverted 2.0m → 1.0m | 2.0m caused transitive CC chains spanning 50m tiles through continuous ground |
| Steal-from-smaller disabled | Punched holes in existing CCs by stealing connective points |

## Reassessment of "fundamental RANSAC limitations"

The prior journal entry concluded that coplanar cross-roof mixing is
"inherent to RANSAC" and cannot be fixed. This was wrong — the limitation was
largely caused by:

1. **Weighted scoring** biasing toward sparse noise (fixed: use count)
2. **Weighted area gate** rejecting legitimate dense CCs (fixed: dual gate)
3. **NaN weights** masking the true behavior (fixed: R2 migration completion)

RANSAC with uniform sampling + count scoring finds 282 planes on Dahouet vs 9
with weighted scoring. The remaining gap vs the hemisphere cascade (which finds
142 segments) is real but much smaller than previously thought, and further
CC/post-processing improvements may narrow it further.
