---
id: "2026-03-12-tile-signals-unified-pca"
date: 2026-03-12
title: "tile-signals: unified full-cloud PCA step"
tags: [tile-signals, eigenvalues, normals, curvatures, pca, kde, preprocessing, exploration]
category: development
status: concluded
refs: ["2026-03-12-spherical-kde-scan-line-compensation"]
pipeline: exploration
---

## Context

The exploration pipeline had three separate tile-wide preprocessing steps that
each built their own kd-tree and ran independent kNN queries on the same points:

1. `compute-tile-normals` — PCA normals at K=16, cascade-compatible artifacts
2. `compute-curvatures` — scattering ratio λ3/(λ1+λ2+λ3) at K=8, 16, 32
3. `compute-eigenvalues` — full eigenvalue decomposition, planarity/linearity/scattering, ternary RGB

This was 3× redundant: same kd-tree build, same kNN lookups, same PCA on
overlapping neighbor sets.

## Design

Merged all three into a single `tile-signals` step:

- **One kd-tree**, built once on all tile points
- **One kNN query at K_MAX=32** per point — neighbor list is sliced for K=8 and K=16
- **3D spherical KDE weighting** at each K slice (see companion entry)
- **Weighted PCA** using KDE-derived inverse-density weights

### Artifacts produced (17 total)

| Artifact | Type | Description |
|----------|------|-------------|
| eigenvalue-l1/l2/l3 | gradient | Raw PCA eigenvalues at K=16 |
| planarity | gradient | (λ2−λ3)/λ1 |
| linearity | gradient | (λ1−λ2)/λ1 |
| scattering | gradient | λ3/λ1 |
| curvatures-k8/k16/k32 | gradient | Scattering ratio λ3/(λ1+λ2+λ3) at each K |
| tile-normals | rgb | Normal map: [-1,1] → [0,255] per component |
| eigen-reliability | gradient | density × spherical isotropy (0–1) |
| eigen-ternary | rgb | R=linearity, G=planarity, B=scattering |
| building-points | metadata | Array<{x,y,z}> — cascade-compatible |
| building-count | metadata | Total point count |
| point-normals | metadata | Array<{x,y,z}> normals — cascade-compatible |
| lidar-density | metadata | pts/m² from XZ bounding box |
| lidar-z-noise | metadata | Default 0.02m |

### Reliability score

Combined metric for each point's PCA quality:

```
reliability = densityFactor × sphericalIsotropy
```

- **densityFactor** = (neighbors within maxRadius) / K — captures spatial sparseness
- **sphericalIsotropy** = inverse Simpson index of 3D KDE weights / n — captures
  directional uniformity (1 = isotropic, 0 = all neighbors in one direction)

### Cascade compatibility

The step produces the same 5 artifacts that `compute-tile-normals` did
(`building-points`, `building-count`, `point-normals`, `lidar-density`,
`lidar-z-noise`), so the downstream tile-wide cascade chain (KDE → ridge →
seed → grow → steal) wires to `tile-signals#0` with zero changes.

## Exploration pipeline wiring

The tile cascade chain now occupies instances #0:

```
tile-signals#0 → kde#0 → crd#0 → seed#0 → cg#0 → rs#0 → cg#1 → ps#0
```

Building cascade (after co-register + scope) was renumbered to #1/#2/#3.
V2 baseline ransac is at rs#1. Pass-2 cascade at kde#2/crd#2/seed#2/cg#4.

## Result

- 3× fewer kd-tree builds per test case
- All PCA-derived signals available for future steps (Axis 10 density watershed)
- RGB visualization of normals and ternary eigenvalue composition in UI
