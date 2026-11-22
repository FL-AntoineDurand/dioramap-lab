---
id: "2026-02-26-pipeline-combination-matrix"
date: 2026-02-26
title: "Pipeline Combination Matrix and Priority Ranking"
tags: [design, pipeline, combination, priority, dependency-graph]
category: design
status: concluded
---

# Pipeline Combination Matrix and Priority Ranking

## Compatibility Table

Which ideas can be combined in a single pipeline, and which conflict.

| Idea | Compatible With | Conflicts With | Notes |
|------|----------------|----------------|-------|
| Ortho ridge confirmation | All | None | Post-processing, independent of seeding strategy |
| BDTOPO metadata priors | All | None | Pre-processing, feeds into branching logic |
| V4+V3 combination | All | None | Pipeline definition, not a step |
| Asymmetric flat stealing | All except footprint-merge | Footprint-merge (overlapping scope) | Both address flat merging; use footprint-merge preferentially |
| Parameter auto-tuning | All | Learning optimal params (replaces) | Auto-tune replaces fixed profiles |
| Multi-resolution RANSAC | All | None | Extension of ransac-seed step |
| Footprint-aware merging | All except LiDAR footprint | LiDAR footprint (no BDTOPO footprint) | Requires BDTOPO footprint |
| LiDAR footprint | Most | Footprint-merge (no BDTOPO footprint) | Changes what "footprint" means downstream |
| Ridge validation | All | None | Complementary to ortho ridge confirmation |
| MBR shape fitting | All | None | Post-processing, conditional per-plane |
| Global direction snap | All | Angle snapping (pick one) | Soft constraint, optional |
| KDE offset histogram | All | None | Alternative density estimation |
| Normal-weighted stealing | All (needs normals) | None | Requires `compute-normals` in pipeline |
| Pre-detect parapets | All | None | Pre-processing step |
| Angle snapping | All | Global direction snap (pick one) | Conditional on BDTOPO metadata |
| Ensemble | All | None | Meta-strategy, runs multiple pipelines |

## Recommended Pipeline Combinations

### Pipeline A: "V4 Complete" (target: best synthetic score)

```
extract-denoise -> compute-normals -> normal-histogram-seed ->
grow -> normal-weighted-steal ->
multi-resolution-ransac-residuals -> grow -> steal ->
categorize -> asymmetric-steal ->
hull -> clip -> footprint-merge -> shape-fit -> resolve-overlaps
```

Expected: 93+ on synth240. Addresses dormer and flat L-shaped.

### Pipeline B: "Multimodal" (target: best real-building performance)

```
extract-denoise -> compute-footprint-from-lidar ->
fetch-metadata -> select-profile ->
compute-normals -> normal-histogram-seed ->
pre-detect-parapets ->
grow -> normal-weighted-steal ->
multi-resolution-ransac-residuals -> grow -> steal ->
categorize -> asymmetric-steal ->
hull -> clip -> shape-fit -> resolve-overlaps ->
compute-ortho-artifacts -> ridge-confirm -> ridge-validate
```

Expected: Best on real buildings (no alignment problem, metadata-guided, validated ridges).

### Pipeline C: "Ensemble" (target: safest, no worst-case regressions)

```
Run Pipeline A + pipeline_v2 + pipeline_v3
Score each with V2 metric (no ground truth needed)
Pick highest-scoring result per building
```

Expected: 93+ on synth240 with no worst-case regressions. ~1.5x runtime cost.

### Pipeline D: "Fast" (target: production batch processing)

```
extract-denoise -> compute-normals -> normal-histogram-seed ->
grow -> steal -> hull -> clip -> resolve-overlaps
```

Expected: 90+ on synth240. Minimal steps, ~50% of Pipeline A runtime.

## Step Dependency Graph

```
extract-denoise ──┬── compute-normals ──── normal-histogram-seed ──┐
                  │                                                 │
                  ├── compute-footprint ──┐                        │
                  │                       │                        │
                  ├── fetch-metadata ─────┤                        │
                  │                       │                        │
                  ├── pre-detect-parapets ┤                        │
                  │                       │                        │
                  │                       v                        v
                  │                  select-profile          competitive-grow
                  │                       │                        │
                  │                       v                        v
                  │                  ransac-seed ──────────── point-stealing
                  │                                                │
                  │                                                v
                  │                                          categorize-planes
                  │                                                │
                  │                                                v
                  │                                          asymmetric-steal
                  │                                                │
                  ├── compute-ortho-artifacts ─────────────────────┤
                  │                                                │
                  │                                                v
                  │                                          hull-extraction
                  │                                                │
                  │                                                v
                  │                                          post-processing
                  │                                                │
                  v                                                v
             ridge-confirm <────────── ridge-validate
```

## Priority Ranking

| Rank | Idea | Impact | Effort | ROI |
|:----:|------|--------|--------|:---:|
| 1 | Footprint-aware plane merging | +20-29 on flat_l_shaped | Low | Very High |
| 2 | Multi-resolution RANSAC | +10-15 on dormer_house | Easy | Very High |
| 3 | Normal-weighted stealing | +0.2 across all (free) | Easy | High |
| 4 | V4+V3 combination | ~92 synth + obstacle metadata | Easy | High |
| 5 | Asymmetric flat stealing | +5-10 on flat_l_shaped | Easy | High |
| 6 | Pre-detect parapets | +2-5 on parapet types | Medium | Medium |
| 7 | Ortho ridge confirmation | +1-3 on cross_plan | Medium | Medium |
| 8 | LiDAR footprint extraction | Large on real buildings | Medium | Medium |
| 9 | BDTOPO metadata priors | +2-5 on specialized types | Medium | Medium |
| 10 | Ensemble methods | +0.5-1 (safety net) | Medium | Medium |
| 11 | Ridge validation | +2-3 on cross_plan | Medium | Medium |
| 12 | Parameter auto-tuning | +0.5-1 overall | Medium | Low |
| 13 | AI segmentation | Unknown (research) | High | Unknown |
| 14 | KDE offset histogram | +0.5-1 on multi-level flats | Easy | Low |
| 15 | MBR shape fitting | +0.5-1 on rectangular roofs | Easy | Low |
| 16 | Global direction snap | +0.1-0.3 (visual quality) | Easy | Low |
| 17 | Angle snapping | +0.1-0.3 (visual quality) | Easy | Low |
