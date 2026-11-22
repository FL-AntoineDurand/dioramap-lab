---
id: "2026-02-24-v2-composite-metrics"
date: 2026-02-24
title: "V2 Composite Metrics: Overshoot and Overlap"
tags: [metrics, v2-composite, overshoot, overlap, polygon-clipping]
category: development
status: concluded
---

# V2 Composite Metrics: Overshoot and Overlap

- Created `geometry-quality.js`: `computeFootprintOvershoot()`, `computePlaneOverlap()` via XZ rasterization
- Added `compositeScoreV2()`: 7 metrics with GT, 5 metrics without GT
- Discovered concave footprint clipping failure (Sutherland-Hodgman only works for convex polygons)
- Researched 6 polygon clipping algorithms; switched to Martinez-Rueda-Feito (`polygon-clipping` npm)
- Building 93006: overshoot 4.22% to 0.00%, V2 82.7 to 83.8 (+1.1)
