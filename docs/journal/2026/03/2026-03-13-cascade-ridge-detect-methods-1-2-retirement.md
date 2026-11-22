---
id: "2026-03-13-cascade-ridge-detect-methods-1-2-retirement"
date: 2026-03-13
title: "cascade-ridge-detect: Methods 1 & 2 are fundamentally wrong — only Method 3 survives"
tags: [ridge, cascade-ridge-detect, density, anti-pattern, finding]
category: finding
status: concluded
refs:
  - "2026-03-02-cascade-step-split"
  - "2026-03-12-local-density-compensation"
pipeline: v7
---

## Context

`cascade-ridge-detect` has three independent methods for tagging ridge
candidates after cascade segmentation. When the tile-wide chain was
introduced in the exploration pipeline, Methods 1 & 2 were silently
disabled (`ridgeNormalEnabled: false`, `ridgeOffsetEnabled: false`) with
only Method 3 (kNN boundary) left active. The rationale was never
documented. This entry reconstructs it.

## Method 3 — the only correct one

**kNN boundary check**: do the point's spatial neighbors (within a
physical radius) belong to 2+ different segments?

This is a direct geometric observation. A point is at a ridge if and
only if the surface changes direction in its immediate physical
neighborhood. Method 3 tests exactly that.

V4 upgraded it from fixed K=8 to `radiusSearch(0.5m)` +
`minNeighbors >= 3` guard — fixing the density anti-pattern where sparse
zones caused the 8 nearest neighbors to span 2–3 meters across a
physical gap to a different plane, producing false ridges deep in plane
interiors.

## Method 1 — wrong: global proxy for a local property

**Normal-space**: is the point's tilt/azimuth within margin of 2+
different orientation peaks in the global histogram?

This tries to detect ridge points by checking if their normal sits
"between" two detected orientations. It fails for two reasons:

1. **Not spatially grounded.** A point can have a normal near two peaks
   because of surface noise, local warping, or simply being on a third
   plane with intermediate orientation — not because it is physically at
   a boundary between two planes.

2. **Global scope.** The peaks come from the entire point set (tile or
   building). Any point whose normal happens to fall in a transition
   zone between *any two planes anywhere* gets flagged. At tile scope,
   this means points on building A get flagged because building B across
   the tile has a similar-but-different orientation.

Method 3 achieves the same intent (find plane-boundary points) but
correctly: it checks physical neighbors, not global histogram proximity.
Method 1 is a strictly worse version of Method 3.

## Method 2 — wrong: confuses offset ambiguity with ridge

**Offset-space**: within the same orientation group, is the point's
perpendicular offset within margin of 2+ different offset peaks?

This is conceptually confused about what a ridge is. Two parallel planes
at different heights in the same orientation group don't form a ridge —
they form a **step or wall**. A ridge is where planes of *different
orientations* meet. A point whose offset falls between two peaks is
simply ambiguously assigned in offset space — it might be noise, a wall
point, or a slightly warped surface. It is not at a ridge.

## Why they existed

Methods 1 & 2 were part of the original monolithic `normal-cascade-segment`
code. When the cascade was split into mode-specific steps + dedicated
`cascade-ridge-detect` (2026-03-02, commit `ac14222`), the three methods
were preserved for behavioral compatibility. The split was explicitly
"behavior-preserving" — no method was questioned, just extracted.

The v4 rewrite (commit `032e041`) added per-method enable flags and
fixed Method 3's density bug, making it possible to disable Methods 1 &
2 without removing code. The exploration pipeline then disabled them for
the tile chain (`cascade-ridge-detect#0`) but the building chains
(`#1`, `#2`) still run all three.

## Decision

- **Method 3 (kNN radius boundary)** is the only correct ridge detection
  method. It should be the only one enabled in all contexts (tile-wide
  and building-scoped).
- **Methods 1 & 2** should remain disabled. They produce false positives
  that downstream steps (competitive-grow, point-stealing, ridge-edge-fit)
  then have to work around.
- The building-scoped cascade chains in exploration still enable Methods
  1 & 2 by default. This should be changed when those chains are migrated
  to V7 dead-end branches — override defaults to kNN-only.

## Anti-pattern: fixed-K kNN in non-uniform density

Method 3's original bug (fixed K=8 reaching across gaps in sparse zones)
is an instance of a general anti-pattern documented in the
density-compensation design entry:

> **Never use fixed-K nearest neighbors for spatial proximity decisions
> in non-uniform density point clouds.** K nearest neighbors defines an
> adaptive-radius ball whose size is inversely proportional to local
> density. In sparse zones, the ball grows large enough to include
> physically distant points from unrelated surfaces. Use
> `radiusSearch(fixed_radius)` + `minNeighbors` guard instead. The
> radius defines a physical neighborhood independent of density, and the
> minNeighbors guard handles the case where the neighborhood is too
> sparse to make a reliable decision (skip rather than guess).

This applies to every algorithm that makes a binary local decision
(ridge/not-ridge, boundary/interior, same-surface/different-surface)
based on neighbor properties. Algorithms that aggregate over neighbors
(PCA, KDE, plane fitting) are less affected because outlier neighbors
are diluted — but they still benefit from density weighting.
