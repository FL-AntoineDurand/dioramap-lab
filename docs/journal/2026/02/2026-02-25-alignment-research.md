---
id: "2026-02-25-alignment-research"
date: 2026-02-25
title: "Alignment Research: 3 Approaches on 41 Real Buildings"
tags: [alignment, icp, dsm, cross-correlation, sobel, morphological]
category: experiment
status: concluded
---

# Alignment Research: 3 Approaches on 41 Real Buildings

## Results

| Approach | Accept Rate | WECS Regressions | Notes |
|----------|-------------|------------------|-------|
| ICP (footprint vs LiDAR convex hull) | 0/41 (0%) | 100% | Diverges: convex hull loses concavities, rotation is wasted DOF |
| DSM cross-corr (ortho vs LiDAR) | 39/41 (95%) | N/A | Sign bug initially masked success |
| DSM cross-corr (footprint vs LiDAR) | 13/41 (32%) | 0% | Best: Amiens WECS 0.48 -> 1.00 |

## DSM Edge Detection Evolution (8 versions)

| Version | Technique | Result | Problem |
|---------|-----------|--------|---------|
| V1 | Sobel on 0-255 greyscale | 59% accept | Display sign bug made it look wrong |
| V2 | Raw Laplacian | Filled blob | LiDAR noise amplified |
| V2b | Gaussian(2) + Laplacian | Still filled | Insufficient smoothing |
| V3 | Gaussian(4) + Sobel + NMS | Too many internal edges | Roof ridges survive |
| V3b | Gaussian(8) + Sobel + threshold | Still internal edges | Multi-level buildings |
| V4a | Binary threshold only | Fragmented | LiDAR gaps create holes |
| V4b | Max-neighbor gap fill | Worse | Spreads building height outward |
| V4c | Close in loop (wrong) | Zero effect | dilate+erode in loop != close(N) |
| **V4d** | **Close(3): dilate 3x then erode 3x** | **Winner** | Fills internal gaps, preserves outline |

## Lesson Learned

V1 Sobel may have been sufficient all along. A viewer sign bug (`-dx` instead of `+dx`) made correct alignments appear wrong, triggering 6 unnecessary edge detection iterations.

- Critical bug found: sign error in viewer ortho-align display (subtracted offset instead of adding)
- Best result: Amiens WECS 0.48 -> 1.00 with footprint cross-correlation
- UI: "Align Sources" + "LiDAR Elevation" toggles in bottom bar
