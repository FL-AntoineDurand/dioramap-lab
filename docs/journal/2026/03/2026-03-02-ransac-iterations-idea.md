---
id: "2026-03-02-ransac-iterations-idea"
date: 2026-03-02
title: "Idea: Adaptive Multi-Wave RANSAC with 800 Iterations"
tags: [ransac, iterations, adaptive, small-features, benchmark]
category: idea
status: proposed
refs: ["2026-02-22-ransac-grow-clip-reseed", "2026-02-26-idea-multi-resolution-ransac"]
pipeline: v2-champion
---

## Motivation

Current RANSAC uses 400 iterations per pass. Cascade (V6) captures major planes via
normal grouping, but on 50% of real buildings (Parisian mansards, half-timbered,
complex multi-face), cascade fails to segment all roof faces. RANSAC must then pick
up the remaining planes from the unassigned point pool.

Small features — chimneys, skylights, vents, dormers — have few inlier points and
are hard to detect with the standard RANSAC parameters. Increasing iterations alone
(400 → 800) gives more random samples but doesn't help if the `minInliers` threshold
is too high to accept these small features.

## Proposal: Adaptive Multi-Wave RANSAC

Instead of a single RANSAC pass with fixed parameters, run multiple "waves" with
progressively relaxed thresholds:

| Wave | Iterations | `minInliersPct` | `distThreshold` | Target |
|------|-----------|-----------------|-----------------|--------|
| 1 | 400 | 0.02 (2%) | 4.5σ | Major planes |
| 2 | 200 | 0.01 (1%) | 6σ | Medium features |
| 3 | 200 | 0.005 (0.5%) | 8σ | Small features (dormers, chimneys) |

Each wave operates on the remaining unassigned points after the previous wave's
grow + steal cycle. The progressive relaxation ensures:
- Wave 1 catches the dominant planes cleanly (tight thresholds)
- Wave 2 relaxes slightly to find medium-sized secondary faces
- Wave 3 aggressively searches for tiny features with loose thresholds

## Benchmark Plan

Use `--param-grid` to sweep:

```bash
node scripts/benchmark.js run \
  --pipeline v2-champion \
  --tc real --all \
  --param-grid '{"iterations": [400, 600, 800], "minInliersPct": [0.02, 0.01, 0.005]}'
```

This generates 9 combinations. Compare composite scores, focusing on:
- Complex roofs (mansards, L-shaped, multi-section)
- Small feature recall (count of detected planes vs ground truth)
- Runtime impact (800 iterations ≈ 2× wall clock vs 400)

## Alternative Approach

Instead of multi-wave RANSAC, improve the cascade to handle these roof types
directly. Better Stage 1 KDE sensitivity (lower `minPeakCount`) could capture
secondary normal clusters that the histogram misses. The cascade approach would
be more principled (physics-based grouping) vs RANSAC's random sampling.

## Decision Criteria

- If multi-wave RANSAC gains ≥3 composite points on complex roofs with <50% runtime increase → adopt
- If cascade improvements achieve similar gains → prefer cascade (deterministic, no random seed dependency)
- Consider hybrid: cascade for major planes, single RANSAC wave with relaxed params for residuals
