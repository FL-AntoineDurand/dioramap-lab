---
id: "2026-02-26-idea-ensemble-methods"
date: 2026-02-26
title: "Idea: Ensemble Methods (Run Multiple Pipelines, Pick Best)"
tags: [idea, ensemble, multi-pipeline, safety-net, research]
category: idea
status: proposed
---

# Idea: Ensemble Methods (Run Multiple Pipelines, Pick Best)

## What
Run 2-3 pipeline variants on each building, score each result using the V2 metric (no ground truth needed), and pick the best.

## Why
Different pipelines excel on different roof types. V2 wins on `multi_section`, V3 on flat with obstacles, V4 on sawtooth. An ensemble could get the best of each.

## How
New runner mode `ensemble`:
1. Run each pipeline (warm cache makes this cheap -- shared preprocessing steps)
2. Compute V2 score for each result (completeness, footprint coverage, residual, overshoot, overlap)
3. Select the result with the highest V2 score
4. Optionally: combine planes from multiple results

## Expected Impact
+0.5-1 pt overall (eliminates worst-case regressions from any single pipeline).

## Difficulty
Medium. Pipeline caching means running 3 variants costs ~1.5x the single-variant cost.

## Timeline
2-3 days.
