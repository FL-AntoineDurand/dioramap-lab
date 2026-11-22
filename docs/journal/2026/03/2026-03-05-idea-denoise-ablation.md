---
id: "2026-03-05-idea-denoise-ablation"
date: 2026-03-05
title: "Idea: Denoising Ablation Experiment"
tags: [extract-denoise, ablation, benchmark, edge-detection]
category: idea
status: proposed
refs: ["2026-03-04-flight-strip-simulation"]
pipeline: v6a-base
---

## Context

`extract-denoise` uses a grid-based spatial filter (radius=0.5 m, minN=3) that disproportionately removes edge points. Analysis of wall-roof-detect v13 showed that denoising kills ~80% of multi-return points (Paris Haussmann: 6160→3341 pts). These are exactly the eave/rupture points most valuable for edge detection.

wall-roof-detect v13 now bypasses the denoised path entirely for its own edge analysis, reading raw building points from `input-lidar`. But the 17 plane-fitting steps still use the denoised output. The question is: **does denoising actually help plane fitting, or does the loss of edge points hurt overall scores?**

## Proposed Experiment

```bash
node scripts/benchmark.js \
  --pipelines v6a-base \
  --test-cases all \
  --param-grid denoiseRadius:0,0.25,0.5,1.0 \
  --no-db
```

Four conditions:
- `denoiseRadius=0` — no denoising at all
- `denoiseRadius=0.25` — mild denoising (keeps more edge points)
- `denoiseRadius=0.5` — current default
- `denoiseRadius=1.0` — aggressive denoising

## Expected Outcome

- If `denoiseRadius=0` scores equal or better → denoising is unnecessary, remove the step
- If `denoiseRadius=0.25` is best → current threshold is too aggressive
- If `denoiseRadius=0.5` is best → status quo confirmed
- Per-category breakdown (synth vs real, simple vs complex) may reveal different optima

## Effort

Low — just run the benchmark command. ~30 min wall time for 643 cases × 4 configs.
