---
id: "2026-03-02-v6x-hybrid-converges-to-v2"
date: 2026-03-02
title: "V6x Hybrid Converges to V2 Champion Results"
tags: [v6, hybrid, convergence, competitive-grow]
category: finding
status: concluded
refs: ["2026-03-02-index-space-mismatch-scorer"]
pipeline: v6x-hybrid
---

# V6x Hybrid Converges to V2 Champion Results

## Question

Why do v2-champion and v6x-hybrid produce identical scores in the UI?

## Setup

Ran both pipelines on 5 synth test cases (gable, hip, l_shaped, mansard, multi_section) with `cache: false`. Compared compositeV2 scores.

## Results

| Test Case | v2-champion | v6x-hybrid | Delta |
|-----------|-------------|------------|-------|
| gable-clean | 36.61 | 36.60 | 0.01 |
| hip-standard | 43.72 | 43.62 | 0.10 |
| l_shaped-clean | 33.99 | 33.97 | 0.02 |
| mansard-clean | 33.46 | 33.06 | 0.40 |
| multi_section-standard | 40.17 | 40.04 | 0.13 |

Both pipelines find the same plane count on every case. Score deltas are 0.01–0.40, invisible at the UI's 1-decimal display (`score.toFixed(1)`).

**Note**: Absolute scores here are wrong due to the segmentation scorer bug. Relative conclusions (convergence) are valid.

## Root Cause

The competitive-grow + point-stealing + refit loop is an equilibrium solver. It converges to the same partition regardless of initial seeds. The v6x cascade pre-grouping (orientation → offset → per-segment RANSAC + CC split) produces different starting seeds than v2's global RANSAC, but the downstream grow/steal loop erases the difference. The RANSAC fallback loop in v6x (`repeat max:3`) further homogenizes.

## Implication

Cascade seeding only matters when it finds planes that RANSAC *cannot* find (e.g. v6a/v6d without a RANSAC fallback dominating). Mixing cascade + full RANSAC fallback is redundant — the RANSAC loop overrides cascade contributions. The v6x "hybrid" design hypothesis (cascade for *what* planes exist, RANSAC for *where*) is invalidated: competitive growing already handles the "where".

## Follow-ups

- v6x-hybrid can be dropped or reclassified as "equivalent to v2"
- Future cascade variants should minimize or remove the RANSAC fallback to preserve cascade-specific gains (as v6a/v6d already do)
- The grow/steal convergence property is worth documenting — it means seed quality matters far less than grow/steal parameters
