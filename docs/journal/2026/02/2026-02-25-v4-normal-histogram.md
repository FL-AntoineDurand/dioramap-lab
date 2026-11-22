---
id: "2026-02-25-v4-normal-histogram"
date: 2026-02-25
title: "V4 Normal Histogram Seeding: 2D Tilt/Azimuth Space"
tags: [v4, normal-histogram, normals, pca, deterministic]
category: development
status: concluded
milestone: "V4 normal histogram"
metrics:
  score: 95.8
  test_suite: "synth10"
pipeline: v4-normal-histogram
---

# V4 Normal Histogram Seeding: 2D Tilt/Azimuth Space

- `compute-normals` step: per-point PCA normals via k-NN
- `normal-histogram-seed` step: 2D histogram in (tilt, azimuth) space, peak detection, d-value sub-clustering
- Results: 9/10 TIE on synth10 (avg 95.8 vs 99.3). Only `multi_section` loses (histogram merges co-oriented spatially separate sections)

## Known Weakness

`multi_section` roof type: when two adjacent roof sections have identical tilt, azimuth, AND d-value AND are XZ-connected, the normal histogram merges them into one plane. RANSAC handles this naturally via random sampling + connected components. This weakness is fundamental to the normal histogram approach.
