---
id: "2026-03-12-spatial-linearity-k-expansion"
date: 2026-03-12
title: "Spatial linearity trigger for adaptive K expansion fixes scan-line false linearity"
tags: [pca, scan-line, knn, linearity, density-compensation, tile-signals]
category: bugfix
status: concluded
refs: ["2026-03-12-cardano-eigensolver", "2026-03-12-tile-signals-unified-pca"]
---

## Problem

LiDAR HD scan patterns create bands of 3 parallel lines (0.15m apart) separated
by 0.5m gaps.  At K=16, the nearest neighbors form a 3m × 0.3m strip — a 10:1
aspect ratio.  This caused **100% false linearity** on perfectly flat surfaces,
because the spatial covariance is dominated by the along-track axis.

The spherical KDE weighting could not compensate because the physical distribution
IS elongated — the weights correct angular clustering but can't fix a 10:1 strip.

### Failed trigger: direction scatter (λ2/λ1 of angular covariance)

The initial approach used direction scatter (v3–v4) to detect 1D neighborhoods.
This failed because 3 lines at 0.15m spacing provide enough **angular** diversity
(dirSpread ~0.5 at K=16) even though the **spatial** distribution is extremely
elongated.  Result: only 0.02% of points triggered expansion on real data.

### Root cause

Direction scatter measures angular coverage, not spatial elongation.  Three lines
within a band create diverse angles to the query point, but the physical extent
is still a narrow strip.  The trigger was blind to the actual problem.

## Solution: unweighted spatial linearity trigger

Replaced the trigger with **unweighted spatial linearity** of the K neighbor
positions: `(λ1 - λ2) / λ1` of the 3D covariance matrix.

This directly measures "are my neighbors arranged in a line?" — exactly the
question that determines whether scan-line bias is present.

| Metric              | Scan-line band (K=16) | Isotropic (K=16) |
|---------------------|-----------------------|-------------------|
| Direction scatter   | 0.43–0.60 (misses!)   | ~0.95             |
| Spatial linearity   | 0.72–0.74 (catches!)  | ~0.05             |

### Algorithm

1. Fetch K_STAGES[0]=32 neighbors for every point
2. Compute unweighted spatial linearity at K=16
3. If linearity > threshold (0.5):
   - Check at K=32 (already fetched) — if still high:
   - Progressively fetch K=48, 64, 96, 128 until linearity drops
   - Stop at the first stage where the neighborhood becomes 2D
4. Compute KDE-weighted PCA on the final expanded neighbor set

### Progressive stages

Instead of a binary 32→128 jump, expansion walks through
`[32, 48, 64, 96, 128]`.  This finds the next band with minimal overshoot:

On Dahouet (45,696 pts): 76.7% expanded
- K32: 25,776 (73%) — most resolved at first stage
- K48: 5,849 (17%)
- K64: 729 (2%)
- K96: 761 (2%)
- K128: 1,920 (5%) — edge/sparse areas only

### Consistency

Expansion applies to ALL signals uniformly — curvatures at k≥16, eigenvalues,
normals, planarity, linearity, scattering, reliability.  Curvature k=8 stays
at 8 nearest (too local to benefit from expansion).

## Test results

Synthetic scan-line band (4 bands × 3 lines × 21 pts = 252 pts, flat Y=5):

| Configuration       | planarity > 0.5 | linearity > 0.5 |
|---------------------|-----------------|-----------------|
| No expansion        | 0.0%            | 100.0%          |
| threshold=0.5       | 84.9%           | 15.1%           |
| threshold=0.2 (agg) | 87.7%           | 12.3%           |

The remaining ~15% with linearity > 0.5 are edge-of-cloud points where even
K=128 can't reach enough cross-band neighbors — genuine sampling limitation.

All 1966 tests pass (32 tile-signals + 43 PCA + rest of suite).
