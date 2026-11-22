---
id: "2026-03-15-ransac-full-density-compensation"
date: 2026-03-15
title: "ransac-full: density compensation deep-dive"
tags: [ransac, density, connected-components, investigation]
category: finding
status: concluded
refs: ["2026-03-13-v7-master-plan"]
pipeline: v7
---

# ransac-full: density compensation deep-dive

## Context

`ransac-full` is a dead-end visualization branch in v7 that runs brute-force RANSAC on all building points, parallel to the hemisphere normal-cascade chain. Its purpose is to compare what pure RANSAC finds vs the normal-cascade approach.

Starting point: version 3 of the step (from prior commit `c0291ec`) found only 11 planes on the `real-22186-dahouet-502` test case (Dahouet), assigning 48% of building points. The hemisphere cascade finds 142 segments on the same tile. Cross-referencing revealed that 20 hemisphere segments with >100 points were completely missed (0% coverage).

Test case characteristics: Dahouet is a coastal village tile with **multiple small buildings**, all with near-horizontal roofs (normalGroup=0). The tile has **large gaps between LiDAR scan bands** (IGN HD airborne LiDAR), making connected component detection fragile.

## Investigation: why planes were missed

### Bug 1: pool flushing discards valid points

**Finding**: after RANSAC finds inliers and CC splits them, ALL inliers were removed from the remaining pool — even those in CCs that were rejected as too small. This is a vicious cycle: RANSAC finds cross-plane inliers (from multiple coplanar roofs) → CC correctly separates them → each fragment is below minSeedArea → rejected → but ALL inliers removed from pool → real planes shrink → repeat until exhausted.

**Fix**: only remove inliers that were actually assigned to a seed plane. Rejected CC points return to the pool for future iterations. Safety guard: if no seeds were produced in an iteration, remove all inliers anyway to avoid infinite loops.

**Result**: 11 → 18 planes, 48% → 54% assigned.

### Bug 2: weighted area gate too strict for dense regions

**Finding**: the `minSeedArea=0.5 m²` gate uses weighted area (`sumArea()` = Σweights / density). Point weights in this tile are wildly uneven: dense scan-band points have weights ~0.01-0.03, while sparse inter-band points have weights up to 4.65. A CC of 450 real building points (hemisphere seg 30) has weighted area of only 0.7 m². Meanwhile, 5 sparse points can weigh 0.45 m².

This means CCs with hundreds of points from a genuine roof face get rejected because their weighted area barely clears the threshold, while isolated sparse points represent disproportionate area.

**Attempted fix**: dual gate — accept CC if weighted area ≥ 0.5 m² OR point count ≥ density × minSeedArea.

**Result**: 18 → 390 planes, 54% → 89% assigned. Zero missed segments. But 390 planes is way too many — the dual gate accepted hundreds of tiny fragments that should have been connected into larger CCs.

**Verdict**: the dual gate treats the symptom, not the cause. The real fix is to make CC connect points properly so large CCs form in the first place. Reverted.

### Root cause: connectDist too small for scan-line gaps

**Finding**: the original CC used `findConnectedComponents(K=15, dist=1.0m)` — a fixed kNN-based approach. Within a scan band, points are 0.03-0.05m apart. But between scan bands, gaps are 1.0-2.0m. With connectDist=1.0m, the CC cannot bridge these gaps, shattering a single roof face into many tiny CCs.

Diagnostic logs confirmed this clearly:
```
RANSAC #7: 2118 inliers (2.25 m²) -> 45 CC (top: 437, 424, 409)
  CC top5 by area: 409p/0.52m², 424p/0.45m², 437p/0.30m²
```
2118 inliers from a real plane split into 45 fragments, largest only 437 points.

### CC experiments

| connectDist | Planes | Assigned | Missed segs |
|---|---|---|---|
| 1.0m (original) | 18 | 54% | 20 |
| 1.45m (8/√density) | 14 | — | 14 |
| 1.81m (10/√density) | 12 | 49% | 10 |
| 2.0m | 12 | 66% | 12 |
| 2.17m (12/√density) | 15 | 65% | 10 |

Increasing connectDist helps the tilted roof segments (normalGroup 2, 5, 6, 8, 9) — they go from 0% to 70-94% coverage. The remaining missed segments are all normalGroup=0 (near-horizontal). These are **ground points around the buildings**, not flat roofs — confirmed by the user. They will never (and should never) be detected by a roof plane detector.

### Adaptive CC attempt (pair-radius from point-weights)

Tried `findConnectedComponentsAdaptive` with per-point radius `ccScale × sqrt(weight) / sqrt(density)`:
- At density 30.5, median weight 0.026, this gives radius = 3 × 0.16 / 5.52 = **0.087m**
- Far too small to connect even adjacent points in the same scan band
- Result: 6107 CCs from 7457 inliers, 0 planes detected

The formula gives the **local point spacing**, which works for competitive-grow (iterative, one-hop-at-a-time growth) but not for CC (needs to bridge multi-hop gaps in one connection step).

### Final solution: kNN + floor(connectDist) + pair-radius

Hybrid approach:
- kNN finds K=15 nearest neighbors (adapts to local density naturally)
- Accept neighbor if `dist ≤ max(connectDist, pairRadius_self, pairRadius_neighbor)`
- `connectDist=2.0m` is the floor — bridges typical scan-line gaps
- pair-radius `= min(ccMax, ccScale × sqrt(w) / sqrt(density))` extends for truly sparse points
- For dense points (majority), the floor dominates; for sparse points, pair-radius kicks in

Result with all fixes combined (give-back + kNN pair-radius CC + grow pass):
- **12 planes**, 25608 seeded (56%) → 30195 after grow (**66%**)
- Tilted segments 49/64/75/46: 74-94% coverage (was 0%)
- Remaining "missed" segments are ground points (correct behavior)

## The grow pass

After all RANSAC iterations, unassigned building points are checked against all detected plane equations. Each point is assigned to the closest plane within `distThreshold`. This fills in gaps within detected planes — points that were on the plane but weren't part of the RANSAC inlier set that iteration.

Result: 56% → 66% assignment (+7527 points recruited). However, the grow pass cannot help segments whose planes were never detected — it only fills in existing planes.

## Remaining limitations

### 1. Coplanar cross-roof mixing (fundamental to RANSAC)

When multiple buildings have roofs at similar height with similar normals (all normalGroup=0), RANSAC finds a single plane equation that fits inliers from ALL of them. CC correctly separates them spatially, but each building's fragment may be small. This is RANSAC's fundamental weakness on multi-building tiles compared to the normal-cascade approach, which groups by spatial proximity within each normal group first.

### 2. No normal pre-grouping

The hemisphere pipeline separates points by normal direction before spatial clustering. This prevents mixing of different roof faces that happen to be coplanar. ransac-full has no such pre-grouping — it operates on all building points at once. This is by design (it's a "brute-force" baseline) but limits its ability to distinguish coplanar faces.

### 3. Weighted area gate sensitivity

The `minSeedArea=0.5 m²` gate uses weighted area, which can be much smaller than naive `count/density` in dense scan bands. A CC with 400 points in a dense band has weighted area ~0.5 m² — barely above threshold. This makes the gate sensitive to weight distribution. Lowering the threshold or using a dual gate (weighted + count) would help but risks accepting noise CCs.

### 4. Grow pass is plane-equation-only (no spatial constraint)

The grow pass assigns points to the nearest plane by equation distance, with no spatial proximity check. A point on building A can be assigned to a plane detected on building B if they're coplanar. This is acceptable for a dead-end visualization branch but would need spatial constraints in a production pipeline.

### 5. Ground points in building-indices (v7 design choice)

In v7, `tileExcludeClasses=null` so ALL tile points are "building" points. This means ground points around buildings are in the pool. They appear as missed normalGroup=0 segments in the cross-reference but are correctly not detected as planes.

## Code changes summary (version 4 → 5)

| Change | Description |
|---|---|
| Pool management | Rejected CC points returned to pool; fallback flush if no seeds |
| CC algorithm | kNN + pair-radius with connectDist=2.0m floor (was fixed kNN dist=1.0m) |
| Grow pass | Post-RANSAC assignment of unassigned points to nearest plane |
| Diagnostic logging | CC size distribution, top-5 by area, rejection counts |
| Parameters | Added connectRadiusScale, connectRadiusMax; connectDist default 1.0→2.0 |

## Verdict

ransac-full is now a reasonable pure-RANSAC baseline. It captures all roof faces including tilted ones (walls, dormers). The main gap vs hemisphere cascade is in multi-building tiles with coplanar roofs — an inherent RANSAC limitation, not a bug. The density compensation stack (weighted sampling → weighted scoring → adaptive CC → grow) is fully in place.
