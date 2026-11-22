---
id: "2026-02-21-gen0-seven-algorithms"
date: 2026-02-21
title: "Gen 0: Seven Exploratory Plane Detection Algorithms"
tags: [gen0, ransac, region-growing, grid, exploration]
category: development
status: concluded
milestone: "Gen 0 complete"
metrics:
  score: 80
  test_suite: "80 synth"
---

# Gen 0: Seven Exploratory Plane Detection Algorithms

## Algorithms Tested

| # | Name | Score | Approach |
|---|------|------:|----------|
| 1 | ransac_peeling | ~70 | Sequential RANSAC: sample 3pts, fit plane, remove inliers, repeat |
| 2 | region_growing_altitude | ~65 | Seed from highest point, grow radially by XZ distance with PCA |
| 3 | grid_region_growing | ~75 | Height grid + Gaussian blur + cell-by-cell growth + merge + fill |
| 4 | normal_region_growing | ~72 | PCA k-NN normals + sort by curvature + BFS growth by normal similarity |
| 5 | cc_ransac | ~76 | RANSAC + connected-component post-processing |
| 6 | cc_ransac_plus | ~80 | + SOR denoising + hull clipping at plane intersection lines |
| 7 | grid_plane_merge | ~78 | Per-cell plane fit + coplanar flood-fill merge |

## Key Insight

RANSAC-based seeding + spatial connectivity checking is essential. cc_ransac_plus combined all four winning elements (RANSAC, connectivity, denoising, hull clipping) and led the generation.

## Weaknesses

All Gen 0 algorithms fail on complex shapes. Ransac_peeling merges disjoint coplanar surfaces. Region growing is seed-order dependent. Grid approaches struggle with non-rectangular shapes. Normal region growing is O(n*k) slow.
