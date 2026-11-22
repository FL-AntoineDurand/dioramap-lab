---
id: "2026-03-06-four-axis-research-synthesis"
date: 2026-03-06
title: "4-Axis LiDAR Plane Detection Research Synthesis"
tags: [research, plane-detection, synthesis, benchmark, comparison]
category: review
status: concluded
refs:
  - "2026-03-06-axis1-radon-plane-detection"
  - "2026-03-06-axis2-multiview-rasterize-detect"
  - "2026-03-06-axis3-spectral-frequency-methods"
  - "2026-03-06-axis4-curvature-pca-region-growing"
---

## Summary

Four autonomous agents explored fundamentally different algorithmic approaches to LiDAR roof plane detection in parallel over ~6 hours. All used the same benchmark harness (159 all-synth cases, 25 quick cases) with IGN LiDAR HD realistic noise.

## Final Scores

| Axis | Approach | Quick | All-Synth | matchRate | Versions |
|------|----------|-------|-----------|-----------|----------|
| **4** | **PCA curvature + region growing** | **98.2** | **96.4** | **96.3%** | 42 |
| 2 | Multi-view rasterize-detect | 97.2 | 95.1 | 93.9% | 10+ |
| 3 | Spectral/normal clustering | 95.1 | 91.3 | 86.2% | 15 |
| 1 | CT/Radon angular sweep | 93.0 | 88.7 | 85.2% | 15+ |

## Head-to-Head (159 cases, Axes 1/2/4 with saved results)

- Axis 4: **90 wins** (57%)
- Axis 2: 51 wins (32%)
- Axis 1: 11 wins (7%)
- Ties: 25 (16%)
- 73 cases have >5pt spread between axes

## Convergent Architecture

All 4 axes independently converged on the same 3-phase pattern:
1. **Normal estimation** (histogram peaks / raster views / spectral clustering / PCA)
2. **Height/offset separation** (offset histograms / Otsu / 1D peak finding)
3. **Spatial CC splitting** (grid flood-fill / raster CC / graph CC / k-NN CC)

## Universal Hard Cases

All axes struggle with the same building types:
- **Row houses**: same pitch, different spatial locations — only CC splitting helps
- **Tiny obstacles** (antennas, vents): <8 points, below any method's floor
- **Parapets**: thin vertical strips that mix with adjacent slopes
- **Cross-plan**: coplanar patches bridged by noisy normals at junctions

## Key Insights

1. **Effort vs return**: Axis 2 reached 95.1 in ~10 versions; Axis 4 needed 42 to reach 96.4. The last 1.3 points cost 4x the exploration effort.
2. **Method complementarity**: Axes fail on different cases — an ensemble could beat any individual.
3. **Spectral methods don't fit**: Graph Laplacian is wrong for spatially smooth manifolds. The pivot to normal clustering was necessary.
4. **Region growing + PCA re-fit is king**: Periodic re-fitting during growth gives the most accurate plane boundaries.

## Recommendations

- **Production**: Adopt Axis 4 (v0.42) as primary, Axis 2 (v1.4) as fast fallback
- **Hybrid potential**: Axis 4 pipeline + Axis 2's offset histogram pre-filter
- **Theoretical ceiling**: ~97-98 all-synth given scorer constraints and noise profile

## Files
- Full report: `research/SYNTHESIS-REPORT.md`
- Per-axis findings: `research/axis-{1,2,3,4}-*/FINDINGS.md`
- Per-axis champions: `research/axis-{1,2,3,4}-*/detector.js`
