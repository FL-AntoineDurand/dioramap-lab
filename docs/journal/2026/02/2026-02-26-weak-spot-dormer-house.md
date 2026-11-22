---
id: "2026-02-26-weak-spot-dormer-house"
date: 2026-02-26
title: "Weak Spot Analysis: Dormer House (~79/100)"
tags: [weak-spot, dormer, minInliers, multi-resolution]
category: finding
status: concluded
---

# Weak Spot Analysis: Dormer House (~79/100)

## Why Weak

Dormer faces are tiny (3 m², ~30 points). The `minInliers` parameter is computed as `minInliersPct * totalBuildingPoints` (typically 0.02 * 5000 = 100 points). Dormers with 30 points are below this threshold and never get seeded by RANSAC.

## Ideas That Could Help

| Idea | Mechanism | Expected Gain | Difficulty |
|------|-----------|:-------------:|:---------:|
| Multi-resolution RANSAC | Third scale with distThreshold=0.05, minInliers=10 | +10-15 | Easy |
| Residual cluster analysis | DBSCAN on unassigned points, fit planes to clusters >15 pts near ridges | +5-10 | Medium |
| Ortho ridge confirmation | Dormer ridges visible in ortho; guide targeted micro-RANSAC | +5 additional | Medium |

## Root Cause

Fixed minimum inlier count. The coarse-to-fine approach is the most direct fix because it introduces a scale-aware threshold.

## Expected Realistic Gain

+10-15 pts with multi-resolution RANSAC alone. +15-21 with ortho guidance.
