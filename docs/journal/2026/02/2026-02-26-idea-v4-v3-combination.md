---
id: "2026-02-26-idea-v4-v3-combination"
date: 2026-02-26
title: "Idea: V4 + V3 Combination (Normal Histogram + Categorization)"
tags: [idea, v4, v3, combination, pipeline]
category: idea
status: superseded
superseded_by: "2026-02-26-v6-normal-cascade-implementation"
---

# Idea: V4 + V3 Combination (Normal Histogram + Categorization)

## What
Combine V4's normal-histogram seeding with V3's early categorization and adaptive RANSAC schedule.

## Why
V4 normal histogram is deterministic and naturally handles gable/hip/mansard/sawtooth. V3 categorization provides obstacle metadata needed for solar.

## How
Define a new pipeline `pipeline_v4_combined`:
```
extract-denoise -> compute-normals -> normal-histogram-seed ->
competitive-grow -> point-stealing ->
repeat [ransac-seed (adaptive) -> grow -> steal] x3 ->
categorize-planes -> asymmetric-steal ->
hull-extraction -> post-processing
```

## Expected Impact
~92 on synth240 with `multi_section` as the known weakness.

## Status
**Superseded by v6e-categorized**, which uses the Normal Cascade (superior to V4 normal-histogram) + V3 categorization + asymmetric-steal.

## Difficulty
Easy. All steps exist; just a new pipeline definition.
