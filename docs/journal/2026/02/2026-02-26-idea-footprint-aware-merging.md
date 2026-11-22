---
id: "2026-02-26-idea-footprint-aware-merging"
date: 2026-02-26
title: "Idea: Footprint-Aware Plane Merging (Highest ROI)"
tags: [idea, footprint, merging, flat-l-shaped, high-roi]
category: idea
status: proposed
---

# Idea: Footprint-Aware Plane Merging (Highest ROI)

## What
Post-processing step that merges spatially disconnected coplanar plane fragments when the building footprint connects them.

## Why
Flat L-shaped buildings get split into 2 fragments by XZ spatial clustering because the narrow connection between the arms falls below the cluster distance threshold. The footprint polygon proves they are the same building.

## How
New step `merge-coplanar-footprint.js`:
1. For each pair of planes: check if coplanar (normal angle < 3 deg, d-value < 0.15m)
2. If coplanar but spatially disconnected: check if the footprint polygon connects their hulls
3. If connected via footprint: merge the two planes (combine point lists, refit plane equation)

## Expected Impact
+20-29 pts on `flat_l_shaped` (71 → 90+). **This is the single highest-impact idea for a specific weak spot.**

## Difficulty
Low (post-processing, no changes to core pipeline).
