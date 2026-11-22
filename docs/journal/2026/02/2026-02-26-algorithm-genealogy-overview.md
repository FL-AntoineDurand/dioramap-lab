---
id: "2026-02-26-algorithm-genealogy-overview"
date: 2026-02-26
title: "Algorithm Genealogy: Parent Tree and Innovation Timeline"
tags: [review, genealogy, lineage, innovation-timeline, parent-tree]
category: review
status: concluded
---

# Algorithm Genealogy: Parent Tree and Innovation Timeline

Complete lineage of every algorithm from project inception (Feb 15, 2026) through V6 Normal Cascade.

## Parent Tree

```
                         ┌─ ransac_peeling ............... Gen 0, ~70, archived
                         ├─ region_growing_altitude ...... Gen 0, ~65, archived
                         ├─ grid_region_growing .......... Gen 0, ~75, archived
  INDEPENDENT ORIGINS ───┤
                         ├─ normal_region_growing ........ Gen 0, ~72, archived
                         ├─ grid_plane_merge ............. Gen 0, ~78, archived
                         └─ ridge_detect ................. Gen 1, ~86 avg, archived
                                                           (ridge-first paradigm, dead end)

  cc_ransac .................. Gen 0, ~76, archived
    └─ cc_ransac_plus ........ Gen 0, ~80, archived
        │
        │  (insight: RANSAC + spatial connectivity + denoising + hull clipping)
        │
        └─ ransac_grow .................. Gen 1, ~83
            │  (breakthrough: competitive region growing + point stealing)
            │
            └─ ransac_grow_clip ............ Gen 1, ~85
                │  (parallel-edge hull clipping at ridges)
                │
                └─ ransac_grow_clip_reseed ...... Gen 1, ~86
                    │  (iterative reseeding from unassigned points)
                    │
                    ├─ ransac_grow_clip_reseed_dilate ... Gen 1, ~85, dead end
                    │
                    └─ V1 CHAMPION: ransac_grow_clip_reseed_corners ... Gen 1, 88.2 (305)
                        │  (exact corner vertices at ridge/footprint intersections)
                        │
                        ├─── Track A: Plane Regularization (Gen 2)
                        │    ├─ A1: pairwise_snap .......... not selected
                        │    ├─ A2: global_direction ....... not selected
                        │    └─ A3: normal_guard ........... not selected
                        │
                        ├─── Track B: Geometric Shape Fitting (Gen 2)
                        │    ├─ B1: min_bounding_rect ..... not selected
                        │    ├─ B2: edge_align ............ not selected
                        │    └─ B4: ridge_constrained ..... SELECTED → V2
                        │
                        ├─── Track C: Obstacle Resilience (Gen 2)
                        │    ├─ C1: height_filter ......... SELECTED → V2
                        │    └─ C3: two_pass .............. not selected
                        │
                        └─ V2 CHAMPION: C1 + B4 .................. Gen 2, 92.0 (240*)
                            │
                            ├─── Track 1: Flat Roof Histogram (Gen 3)
                            │    ├─ 1a: flat_peaks ......... SELECTED → V3
                            │    ├─ 1b: flat_kde ........... not selected
                            │    ├─ 1c: flat_spikes ........ REJECTED
                            │    └─ 1d: flat_hybrid ........ not selected
                            │
                            ├─── Track 2: Normals Reuse → NOT SELECTED
                            ├─── Track 3: Early Categorization → SELECTED → V3
                            ├─── Track 4: Adaptive RANSAC → SELECTED → V3
                            ├─── Track 5: Parapet-Aware → NOT SELECTED
                            │
                            └─ V3 COMBINED: 1a + 3 + 4 ............ Gen 3, 91.9 (240)
                                │
                                │  ┌─── PIPELINE ARCHITECTURE (Gen 4) ───┐
                                │  │  27 monolithic → 24 composable steps │
                                │  └──────────────────────────────────────┘
                                │
                                ├─ pipeline_v2 ............... Gen 4, verified = V2
                                ├─ pipeline_v3 ............... Gen 4, verified = V3
                                ├─ pipeline_v4 ............... Gen 4, normal histogram
                                ├─ pipeline_v2_co_register ... Gen 4, V2 + ICP
                                ├─ pipeline_v2_dsm_coreg ..... Gen 4, V2 + DSM cross-corr
                                ├─ pipeline_v5_multimodal .... Gen 5, full multimodal
                                │
                                └─── V6 NORMAL CASCADE ─── Gen 6 (current frontier)
                                      ├─ v6a-base ............ 132W/38L vs v2
                                      ├─ v6b-ridge-aware
                                      ├─ v6c-adaptive
                                      ├─ v6d-progressive ..... 203W/46L vs v2
                                      ├─ v6e-categorized
                                      └─ v6f-minimal
```

## Innovation Timeline

Every significant algorithmic idea, chronologically. "Survived?" indicates whether it persists in the current best pipeline.

| Date | Innovation | Introduced In | Survived? |
|------|-----------|---------------|-----------|
| Feb 20 | Synthetic test case generator (30 roof types) | Framework | Yes |
| Feb 20 | V1 composite scoring (5 metrics) | Framework | Yes |
| Feb 21 | RANSAC 3-point sampling + PCA refine | Gen 0 | Yes |
| Feb 21 | SOR denoising | cc_ransac_plus | Yes (as grid-based) |
| Feb 21 | Connected-component post-processing | cc_ransac | Yes |
| Feb 21 | Hull clipping at plane intersection lines | cc_ransac_plus | Yes |
| Feb 21 | **Competitive region growing + point stealing** | **ransac_grow** | **Yes (core)** |
| Feb 21 | Steal threshold ratio (0.80) | ransac_grow | Yes |
| Feb 21 | Parallel-edge hull clipping at ridges | ransac_grow_clip | Yes |
| Feb 21-22 | Iterative reseeding from unassigned points | ransac_grow_clip_reseed | Yes |
| Feb 22 | Exact corner vertex computation | V1 Champion | Yes |
| Feb 22 | `isOnCorrectSide()` prefilter | V1 Champion | Yes |
| Feb 22 | `clipPlanesToFootprint` (Martinez-Rueda-Feito) | V1 Champion | Yes |
| Feb 22 | Hull dilation approach | dilate variant | **No** |
| Feb 22 | Ridge-first detection (Hessian) | ridge_detect | **No** |
| Feb 24 | V2 composite scoring (7 metrics with GT) | Session 14 | Yes |
| Feb 24 | `resolveOverlappingPlanes()` | Session 15 | Yes |
| Feb 24 | 240-case expanded test suite | Session 16 | Yes |
| Feb 24 | **Ridge-constrained shape fitting** | **Track B4** | **Yes** |
| Feb 24 | **Obstacle height filter** | **Track C1** | **Yes** |
| Feb 24-25 | **Flat roof histogram seeding** | **Track 1a** | **Yes (V3)** |
| Feb 24-25 | **Early plane categorization (6 types)** | **Track 3** | **Yes** |
| Feb 24-25 | **Asymmetric steal rules** | **Track 3** | **Yes** |
| Feb 24-25 | **Adaptive RANSAC parameter schedules** | **Track 4** | **Yes (V3)** |
| Feb 25 | **Bug fix: overlap-resolve sort by point count** | Session 19 | **Yes** |
| Feb 25 | **Composable pipeline architecture (24 steps)** | Pipeline refactor | **Yes** |
| Feb 25 | **Step-level caching with chain-hash** | Pipeline runner | **Yes** |
| Feb 25 | **k-NN PCA per-point normals** | **V4** | **Yes** |
| Feb 25 | **2D (tilt, azimuth) normal histogram seeding** | **V4** | **Yes** |
| Feb 25 | ICP 2D rigid alignment | Co-registration | Yes (fallback) |
| Feb 25 | WECS metric | Co-registration | Yes |
| Feb 25 | DSM rasterization + morphological edge detection | DSM artifacts | Yes |
| Feb 25 | DSM edge cross-correlation alignment | DSM co-reg | Yes |
| Feb 26 | **Offset projection along group normal** | **V6 cascade** | **Yes** |
| Feb 26 | **Three-stage cascade: orientation → offset → spatial** | **V6 cascade** | **Yes** |
| Feb 26 | **Parapet pre-detection via separate normal group** | **V6 cascade** | **Yes** |
| Feb 26 | **3×3 smoothed histogram peak detection** | **V6 cascade** | **Yes** |
| Feb 26 | **Flat-roof azimuth aggregation** | **V6 cascade** | **Yes** |
| Feb 26 | **Per-group RMS adaptive offset bins** | **V6c** | **Yes** |
| Feb 26 | **Progressive two-pass cascade** | **V6d** | **Yes** |
