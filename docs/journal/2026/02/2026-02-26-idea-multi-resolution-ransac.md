---
id: "2026-02-26-idea-multi-resolution-ransac"
date: 2026-02-26
title: "Idea: Multi-Resolution RANSAC (Coarse-to-Fine)"
tags: [idea, ransac, multi-resolution, coarse-to-fine, dormer]
category: idea
status: proposed
---

# Idea: Multi-Resolution RANSAC (Coarse-to-Fine)

## What
First pass: aggressive RANSAC with large distThreshold (0.25m) and high minInliers (50). Second pass: tight RANSAC (0.05m, minInliers 10) on residual points to find small features (dormers, vents, small offsets).

## Why
Current RANSAC uses a single threshold across all iterations. Large features are easy to find but waste iterations. Small features need tight thresholds but get drowned out by noise.

## How
Modify `ransac-seed.js` to accept a `scales` parameter:
```
scales: [
  { distThreshold: 0.25, minInliers: 50, maxIter: 500 },
  { distThreshold: 0.10, minInliers: 15, maxIter: 1000 },
  { distThreshold: 0.05, minInliers: 10, maxIter: 500 },
]
```
Each scale runs on points not yet assigned by previous scales.

## Expected Impact
+5-10 pts on `dormer_house` (finds tiny dormer faces). +2-3 pts on `flat_with_chimney`.

## Difficulty
Easy (parameter extension of existing step).
