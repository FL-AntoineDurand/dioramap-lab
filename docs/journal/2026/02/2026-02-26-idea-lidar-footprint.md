---
id: "2026-02-26-idea-lidar-footprint"
date: 2026-02-26
title: "Idea: Recompute Footprint from LiDAR (Skip BDTOPO)"
tags: [idea, footprint, lidar, alpha-shape, concave-hull, alignment]
category: idea
status: proposed
---

# Idea: Recompute Footprint from LiDAR (Skip BDTOPO)

## What
Instead of using the BDTOPO cadastral footprint (which has 0.5-2.0m offset), compute the building footprint directly from the LiDAR point cloud using alpha shapes or concave hull.

## Why
Eliminates the alignment problem entirely. The co-registration step (ICP, DSM cross-correlation) is complex and only accepts 34-59% of cases. A LiDAR-derived footprint is inherently aligned.

## How
New step `compute-footprint.js`:
1. Take building-classified points (after extract + denoise)
2. Project to XZ plane
3. Compute concave hull (alpha shape with alpha = 1-2m)
4. Simplify polygon (Douglas-Peucker, 0.3m tolerance)
5. Output as footprint polygon, replacing BDTOPO footprint in subsequent steps

## Expected Impact
Potentially large on real buildings where BDTOPO footprint is misaligned. On synthetic cases: no impact. On real buildings: could improve hull clipping, corner gap filling, and footprint coverage metrics across the board.

## Difficulty
Medium (alpha shape computation, need to tune alpha parameter).
