---
id: "2026-02-26-idea-global-direction-clustering"
date: 2026-02-26
title: "Idea: Global Direction Clustering (Soft Constraint)"
tags: [idea, direction, clustering, snap, regularization, retry]
category: idea
status: abandoned
---

# Idea: Global Direction Clustering (Soft Constraint)

## Original Attempt
Track A2 (`reg_a2_global_direction`) — cluster all plane normals, snap to canonical tilt/azimuth directions (0/15/30/45 deg).

## Why It Failed
Score decrease on non-regular buildings.

## Why Retry
Instead of forcing canonical angles, use global direction clustering as a SOFT constraint: compute the 2-3 dominant azimuth directions, and only snap planes that are within 3 deg of a canonical direction. Combine with BDTOPO material priors.

## How to Retry
Modify `shape-fitting.js` to optionally snap plane azimuths when `abs(azimuth - nearest_canonical) < 3 deg`.

## Expected Impact
+0.1-0.3 pts on regular buildings. Main value is visual quality (cleaner ridge lines for solar panel layout).

## Difficulty
Easy.
