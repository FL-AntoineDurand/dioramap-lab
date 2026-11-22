---
id: "2026-03-12-spherical-kde-scan-line-compensation"
date: 2026-03-12
title: "3D spherical KDE for LiDAR scan-line anisotropy compensation"
tags: [kde, pca, eigenvalues, anisotropy, scan-lines, lidar-physics, tile-signals]
category: finding
status: concluded
refs: ["2026-03-12-tile-signals-unified-pca"]
---

## Problem

LiDAR HD acquisition creates anisotropic point distributions. The scanner
sweeps perpendicular to the flight path, producing dense scan lines with
sparser gaps between them. When PCA is computed on a local kNN neighborhood,
this elongated distribution biases the covariance matrix:

- **False linearity**: flat surfaces show high linearity because λ1 >> λ2
  (neighbors stretch along the scan line)
- **Suppressed planarity**: the second eigenvalue is artificially small
- **Unreliable normals**: the smallest eigenvector (normal) wobbles because
  λ2 ≈ λ3 instead of λ2 >> λ3

## Why 2D horizontal KDE fails

The initial approach projected all neighbors onto the XZ ground plane and
measured angular clustering with a 1D circular Gaussian KDE. This works on
flat surfaces but degrades on slopes:

- On a 45° roof, the scan line follows the slope — its dense direction is
  tilted 45° from horizontal. The 2D projection collapses the vertical
  component, underestimating the true angular clustering.
- On a vertical wall, all neighbors project to nearly the same XZ point
  regardless of scan-line structure.
- PCA operates in 3D; compensating in 2D leaves the 3D bias uncorrected.

## Solution: 3D spherical KDE

For each query point's K neighbors, compute unit direction vectors on the
unit sphere, then estimate directional density using a Gaussian kernel on
squared chord length (equivalent to von Mises-Fisher up to normalization):

```
v_j = normalize(p_j - q_i)           // unit direction to neighbor j
chordSq(j,k) = |v_j - v_k|²         // ≈ angle² for small angles
density_j = Σ_k exp(-chordSq(j,k) / (2σ²))
weight_j = 1 / density_j             // inverse density
```

Weights are normalized to sum to 1, then used for weighted centroid and
weighted covariance in the PCA.

### Why chord length instead of arc length

For two unit vectors, `|v_j - v_k|² = 2(1 - cos θ)`. For small angles
(θ < 60°, covering virtually all kNN pairs), this approximates θ² well.
The kernel becomes:

```
K(θ) = exp(-(1 - cos θ) / σ²) = C · exp(cos θ / σ²)
```

This is exactly a **von Mises-Fisher kernel** with concentration κ = 1/σ².
The constant C cancels in weight normalization. No `acos()` needed —
just dot products and subtractions.

### Bandwidth

σ = π/8 ≈ 22.5° → κ ≈ 6.5. At this concentration:
- Same-direction neighbors (θ ≈ 0°): kernel ≈ 1.0
- 45° apart: kernel ≈ 0.05
- 90° apart: kernel ≈ 0.002 (negligible)

This resolves scan-line clustering (typically < 30° angular spread) while
not over-penalizing moderate angular variation.

### Isotropy metric

The inverse Simpson index of normalized KDE weights measures directional
uniformity:

```
isotropy = (1 / Σ(w_j²)) / n
```

- isotropy = 1: all weights equal (perfectly isotropic neighborhood)
- isotropy → 1/n: one dominant weight (all neighbors in one direction)

Combined with spatial density factor for the final reliability score.

## Complexity

O(K²) per point — same as the 2D version. The inner loop replaces
`atan2 + circular_diff` with `3 subtractions + 3 multiplies + 2 adds`
(chord squared). Slightly faster per iteration despite being 3D.

## Impact

- Flat surfaces: planarity correctly high, linearity low (scan-line bias removed)
- Steep roofs: no degradation — scan-line direction detected in 3D
- Walls: still functional (2D version would fail completely)
- Ridges/edges: linearity remains genuinely high (real geometric feature, not artifact)
