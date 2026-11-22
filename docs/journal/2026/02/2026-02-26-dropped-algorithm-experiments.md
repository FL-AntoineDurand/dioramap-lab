---
id: "2026-02-26-dropped-algorithm-experiments"
date: 2026-02-26
title: "Dropped Algorithm Experiments: Definitively and Marginally"
tags: [review, dropped, dead-end, marginal]
category: review
status: concluded
---

# Dropped Algorithm Experiments

## Definitively Dropped (Confirmed Inferior)

| Idea | Tried In | Why Dropped | Worth Revisiting? |
|------|----------|-------------|-------------------|
| **Hull dilation + clip** | `ransac_grow_clip_reseed_dilate` | Less precise than exact corner computation; L/T-shaped trade-offs | No. Corner approach is strictly better. |
| **Ridge-first paradigm** (Hessian + flood fill) | `ridge_detect` | Catastrophic failure on hip roofs (44.8). Ridges don't form complete barriers. | No for primary algo. Maybe as validation. |
| **Global normal clustering + canonical snap** (A2) | `reg_a2_global_direction` | Actively hurts on non-regular buildings. | No. |
| **Minimum bounding rectangles** (B1) | `shape_b1_mbr` | Over-regularizes non-rectangular planes. -0.3 on 10-case. | No. |
| **Spike/floor binary classification** (1c) | `flat_spikes` | Regression -1.2 pts. | No. |
| **Standalone parapet detection** (Track 5) | `parapet_aware` | Completely subsumed by Track 3's categorization. | No. |
| **ICP for footprint alignment** | `co-register (icp)` | 0/41 accepted on real buildings. | No for footprint alignment. |
| **Max-neighbor gap fill for DSM** | DSM edge V4b | Spreads building height outward. | No. |
| **Close-in-loop (wrong morphological close)** | DSM edge V4c | Zero effect. Bug, not a technique. | No. |

## Dropped Due to Marginal Improvement (Could Revisit)

| Idea | Tried In | Why Dropped | Worth Revisiting? |
|------|----------|-------------|-------------------|
| **Pairwise angle snapping** (A1) | `reg_a1_pairwise_snap` | +0.0 on 305-case. Already accurate. | Maybe with BDTOPO priors. Low priority. |
| **Normal deviation guard** (A3) | `reg_a3_normal_guard` | +0.0. PCA normals add runtime for no benefit. | The normals survived into V4. Guard itself probably not. |
| **Edge alignment** (B2) | `shape_b2_edge_align` | +0.1 on 10-case. Marginal. | Maybe as B4 refinement. Low priority. |
| **Gaussian KDE for elevation peaks** (1b) | `flat_kde` | Same score as binning. More complex. | Only if binning fails on real cases. |
| **Hybrid histogram + RANSAC** (1d) | `flat_hybrid` | 88.2, lower than 1a (88.5). | No. |
| **Normal-weighted stealing** (Track 2) | `normals_enhanced` | +0.2 but +5% runtime. | **Yes, in V4+ pipelines** where normals are already computed. Zero extra cost. |
| **Roof type classification from normals** (Track 2) | `normals_enhanced` | Not directly useful for scoring. | Yes, for metadata/branching. |
| **Ridge detection via normal variance** (Track 2) | `normals_enhanced` | Overshadowed by ortho edge detection. | Maybe as LiDAR-only ridge signal. |
| **Two-pass obstacle classification** (C3) | `obstacle_c3_two_pass` | Same score as C1, more complex. | No. |
