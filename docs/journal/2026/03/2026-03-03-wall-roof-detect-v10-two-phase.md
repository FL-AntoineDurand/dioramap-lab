---
id: "2026-03-03-wall-roof-detect-v10-two-phase"
date: 2026-03-03
title: "wall-roof-detect v10: Two-Phase Edge Fitting with Mini-Cascade"
tags: [pipeline, edge-detection, wall-roof, kde]
category: development
status: concluded
refs: []
pipeline: v6g-kde-full
---

## Problem

wall-roof-detect v8 grouped candidates by gap direction bin, clustered in XZ,
then split upper/lower by gapMidY. This had two major issues:

1. **Gable merge**: Two symmetric slope faces share the same XZ location and
   direction bin, so they merged into a single horizontal edge at the mean height
   instead of two tilted edges following each roof slope.

2. **XZ position drift**: Free 3D PCA fitting let roof edges drift inward and
   ground edges drift outward from the true wall position.

## Solution: Two-Phase Architecture (v9 -> v10)

### Phase 1 — XZ Footprint Edges

Project all candidates to XZ ground plane, group by direction bin (8 x 45deg),
cluster with `findConnectedComponents`, fit 2D PCA lines. This locks the XZ
position and orientation of each wall segment.

### Phase 2 — Mini-Cascade in (t, Y) Wall Plane

For each footprint edge, project candidates to wall-local coordinates:
- `t` = projection along footprint direction (along-wall distance)
- `Y` = original height

Three-stage cascade:
1. **Slope angle KDE** — local slope theta via k-NN PCA in (t,Y), then KDE on
   theta values. Separates different roof faces (e.g., +30deg vs -30deg on a gable).
2. **Perpendicular offset KDE** — within each slope group, compute perpendicular
   offset and KDE to separate parallel edges at different heights (sawtooth).
3. **Spatial CC** — connected components in (t,Y) for continuity.

### XZ-Constrained Fitting

Phase 2 only determines `dY/dt` (slope) via linear regression. The 3D direction
is `normalize(d2D.x, slope, d2D.y)` — XZ orientation locked to Phase 1. The
centroid walks along the footprint line by `meanT`, keeping XZ position on the
wall.

### Small-Group Fast Path

Groups with `< 3 * minCluster` candidates skip slope/offset KDE and go straight
to spatial CC + constrained fit. Prevents over-fragmentation of small structures
(towers, chimneys).

### Relaxed Post-Cascade Thresholds (v10)

After the cascade ensures slope+offset homogeneity, spatial CC uses relaxed
thresholds:
- `postCascadeRadius = radius * 3` (1.5m vs 0.5m) — adjacent candidates are
  ~0.58m apart in (t,Y) due to slope, exceeding the base radius.
- `postCascadeMinPts = max(5, ceil(minCluster/2))` — 5 coplanar connected
  points is a meaningful edge after 3 cascade stages.

Without this, tower faces (59-64 candidates) were fragmented into components
of 3-7 points, all below minCluster=10, producing 0 edges.

## Results

### Church-medium (proc-church-medium-standard)

| Metric | v8 | v10 |
|--------|-----|-----|
| Tower face FE#1 (59 pts) | 0 edges | 3 edges |
| Tower face FE#3 (64 pts) | 1 edge | 4 edges |
| Total raw edges | ~12 | 45 |
| Final edges (after merge) | ~8 | 23 |

### Gable Benchmark (24 cases)

| Metric | v8/v9 | v10 |
|--------|-------|-----|
| Pass rate | 24/24 | 24/24 |
| Avg score | 92.1 | 92.1 |
| Avg time | 24s | 35s |

Score-neutral on all gable cases. Timing increase from larger CC radius creating
more connections to evaluate.

## New Param

| Param | Default | Description |
|-------|---------|-------------|
| `slopeBinDeg` | 5 | KDE bandwidth floor for slope angle segmentation (degrees) |

## Files Changed

- `lib/pipeline/steps/wall-roof-detect.js` — v8 -> v10 (complete Phase 2 rewrite)
- `test/edge-detection.test.js` — version check updated
- `docs/STEP-CATALOG.md` — updated description
