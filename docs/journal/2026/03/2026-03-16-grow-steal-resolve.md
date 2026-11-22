---
id: "2026-03-16-grow-steal-resolve"
date: 2026-03-16
title: "grow-steal-resolve: unified plane expansion with equation refit and spatial coherence"
tags: [segmentation, growing, stealing, resolve, v7]
category: development
status: concluded
refs:
  - "2026-03-16-lambert-antipodal-normal-merge"
  - "2026-03-15-ransac-density-compensation-revisited"
pipeline: v7
---

# grow-steal-resolve: unified plane expansion

## Motivation

The cascade segmentation pipeline (hemisphere → normal-cascade-seed →
competitive-grow → point-stealing) produces false small planes near ridges
and valleys. The root cause: kNN normal estimation smooths normals across
ridge edges, creating a gradient of tilts (e.g. 4°→28°→49°→56°) where the
true surface is flat. The cascade correctly detects different tilts as
different normal groups, but each fragment is too small to be useful and
none can be absorbed by standard competitive-grow or point-stealing.

### Why competitive-grow can't fix this

Competitive-grow uses **fixed** plane equations from seeds. Each seed grows
outward claiming unowned points that fit its equation. But seed points are
already owned — plane A can't claim points owned by plane B. The small
fragments are permanently locked in.

### Why point-stealing can't fix this

Point-stealing reassigns boundary points to a neighbor plane if the neighbor
fits better. But each small fragment's equation fits its own points better
than any neighbor's equation (it was fitted to exactly those points). The
residual advantage is real but tiny — not enough to trigger a steal, and
even with stealThreshold=1.0 (steal on any improvement), the fragments
keep their points because `distance(point, ownPlane) < distance(point, neighborPlane)`.

## Design

A unified step that replaces competitive-grow + point-stealing with 4 phases:

### Phase 1: Aggressive per-plane growth

Each plane takes a turn growing outward from its seed, claiming ANY
neighboring point (owned or unowned). The equation **refits** as the plane
absorbs new points, adapting to the actual surface. All planes grow
independently — overlapping claims are allowed.

Key parameters:
- **acceptRadius**: `max(P95(seed residuals), distThreshold)` — adapts to
  plane noise level. Clean planes (P95=0.03) use distThreshold floor.
  Noisy planes (P95=0.12) accept wider.
- **RMS stop**: `baseline + maxDegradation` where maxDegradation = 2×zNoise.
  Growth stops when the refitted plane degrades too much.
- **Spatial connectivity**: each claimed point must have at least one already-
  claimed neighbor (prevents jumping across gaps).

Density compensation: adaptive search radius from point-weights (sparse points
search wider). Pair-radius rule for connectivity.

### Phase 1b: Brutal merge

After all planes grow, detect pairs with heavy overlap (>30% of the smaller
plane's claims shared). Try fitting one plane to all combined points. If
RMS ≤ limit, merge the small plane into the larger one.

On Dahouet: merged plane 42 (33pts) into plane 38, plane 37 (41pts) into
plane 2, and 2 others. Total 4 merges.

### Phase 2: Resolve overlapping claims

Each point may be claimed by multiple planes. Assign to the **largest
claimant** — a smaller plane only wins if its residual is less than 50%
of the larger plane's residual. This prevents small blurred-normal fragments
from keeping contested points.

### Phase 2b: Spatial coherence (majority vote)

For each point, check its spatial neighbors. If >70% belong to a different
plane AND that plane fits within distThreshold, switch. Iterates until
stable (typically 3-4 rounds).

This is a median-filter on the segmentation mask. It dissolves isolated
fragments surrounded by a larger plane, even when their own equation fits
marginally better. The spatial evidence (15 neighbors on plane B vs 2 on
plane A) overrides the tiny residual advantage.

On Dahouet: 100 points moved across 4 rounds (87→8→3→2). Plane 32
reduced from 28 to 17 points.

### Phase 3: Dissolve + final refit

Planes below minPlaneSize (15) are dissolved. Their points get late-assigned
to the nearest neighbor plane within distThreshold. All surviving planes
get a final least-squares refit on their resolved point set.

## Results on Dahouet

| Plane | Before (cascade seeds) | After GSR |
|-------|----------------------|-----------|
| 32 | 28 pts (tilt=28°, false fragment) | 17 pts (eroded by coherence) |
| 37 | 41 pts (tilt=4°, near-flat fragment) | 0 pts (merged into plane 2) |
| 38 | 37 pts (tilt=49°, the real roof face) | 112 pts (grew + absorbed 42 + coherence) |
| 42 | 33 pts (tilt=56°, false fragment) | 0 pts (merged into plane 38) |

Total: 56 active planes (of 60 seeds), 4 merges, 100 coherence moves.

## Density compensation

Following arena rules §4:

| Operation | Uses weights? | Rationale |
|-----------|:---:|-----------|
| Spatial search radius | Yes | Sparse → wider (pair-radius) |
| Growth frontier priority | No | Residual distance (measurement-based) |
| RMS computation | No | Standard least-squares (weighted refit uses fitPlaneLeastSquares) |
| Size gates | Yes (dual) | Area OR count |
| Accept/reject per point | No | Residual vs distThreshold |

## Key insight

The fundamental innovation is **equation refit during growth**. Previous
steps (competitive-grow, point-stealing) use fixed equations — they can
only assign points that already fit the seed equation. GSR's equation
adapts as it grows, which means a roof face that starts as a 37-point
seed at tilt=49° can absorb adjacent points and refit to a 112-point
surface that includes the blurred-normal zone. The refitted equation is
a better fit for the combined surface than any of the individual fragment
equations were.
