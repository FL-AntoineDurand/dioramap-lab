---
id: "2026-02-26-score-evolution-per-roof-type"
date: 2026-02-26
title: "Score Evolution: Per-Roof-Type Breakdown (V2 Champion)"
tags: [benchmark, score, per-roof-type, evolution, weak-spots]
category: benchmark
status: concluded
---

# Score Evolution: Per-Roof-Type Breakdown

## By Generation (Best Algorithm Per Generation)

| Gen | Date | Algorithm | Score | Test Suite | Key Delta |
|:---:|------|-----------|------:|:----------:|-----------|
| 0 | Feb 21 | cc_ransac_plus | ~80 | 80 synth | Baseline |
| 1 | Feb 21 | ransac_grow | ~83 | 80 synth | +3 (competitive growing) |
| 1 | Feb 21 | ransac_grow_clip | ~85 | 80 synth | +2 (parallel-edge clip) |
| 1 | Feb 21-22 | ransac_grow_clip_reseed | ~86 | 80 synth | +1 (reseeding) |
| 1 | Feb 22 | **V1 Champion** (corners) | **88.2** | 305 synth | +2 (corner gaps) |
| 2 | Feb 24 | **V2 Champion** (C1+B4) | **88.4** | 305 synth | +0.2 (obstacle filter + shapes) |
| 2* | Feb 25 | V2 (post-fix, new suite) | **92.0** | 240 synth | +1.0 bug fix + different suite |
| 3 | Feb 24-25 | V3 Combined | **91.9** | 240 synth | -0.1 (trade-off for real perf) |
| 4 | Feb 25 | pipeline_v4 | 95.8 avg | synth10 | 9/10 TIE |
| 6 | Feb 26-27 | **v6a-base** | **132W/38L** | 643 all | 3.5:1 win rate |
| 6 | Feb 27 | **v6d-progressive** | **203W/46L** | 643 all (pre-fix) | 4.4:1 win rate |

## Per-Roof-Type Scores (V2 Champion, 240 expanded, post-fix)

| Roof Type | V2 Score | Category |
|-----------|:--------:|:--------:|
| gable | 99.3 | Easy |
| hip | 98.9 | Easy |
| asymmetric_gable | 99.3 | Easy |
| l_shaped | 99.1 | Easy |
| t_shaped | 99.3 | Easy |
| sawtooth | 99.4 | Easy |
| flat | 99.6 | Easy |
| mono_pitch | 99+ | Easy |
| flat_large_commercial | 99.4 | Easy |
| industrial_stepped_* | 99.3-99.6 | Easy |
| mansard | 93.6 | Medium |
| multi_section | 95+ | Medium |
| flat_with_chimney | 88.7 | Hard |
| flat_with_ac_unit | 88.0 | Hard |
| mono_pitch_with_parapet | 86.7 | Hard |
| cross_plan | 83-85 | Hard |
| flat_large_parapet | 79.0 | Hard |
| dormer_house | ~79 | Hard |
| flat_l_shaped | ~71 | Hard |
| **row_houses** | **~50** | **Unsolved** |

## Benchmark Result Files (V1 repo)

| File | Date | Description |
|------|------|-------------|
| `compare-2026-02-25T07-32-24.json` | Feb 25 07:32 | **V3 final: 3 algos, 240 cases (post-fix definitive)** |
| `compare-2026-02-24T18-06-00.json` | Feb 24 18:06 | V2 full: 7 algos, 305 cases |
| `compare-2026-02-24T21-57-08.json` | Feb 24 21:57 | V3 pre-fix: 10 algos, 305 cases |
