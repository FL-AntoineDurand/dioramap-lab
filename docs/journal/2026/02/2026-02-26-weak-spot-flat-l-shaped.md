---
id: "2026-02-26-weak-spot-flat-l-shaped"
date: 2026-02-26
title: "Weak Spot Analysis: Flat L-Shaped (~71/100)"
tags: [weak-spot, flat-l-shaped, clustering, footprint, merge]
category: finding
status: concluded
refs: ["2026-02-26-idea-footprint-aware-merging", "2026-02-26-idea-asymmetric-flat-stealing"]
---

# Weak Spot Analysis: Flat L-Shaped (~71/100)

## Why Weak

A single flat plane covers an L-shaped footprint. XZ spatial clustering (in normal-histogram-seed or RANSAC connected components) splits it into 2 disconnected regions because the connection between the L arms is narrow (1-2m wide, below cluster distance threshold).

## Ideas That Could Help

| Idea | Mechanism | Expected Gain | Difficulty |
|------|-----------|:-------------:|:---------:|
| Footprint-aware plane merging | Merge coplanar fragments connected through footprint polygon | +20-29 | Low |
| Asymmetric flat stealing | Larger flat fragment absorbs smaller coplanar fragment | +5-10 | Easy |
| Relaxed clustering distance | Increase XZ cluster distance to 3-4m for flat planes specifically | +5-10 | Easy |

## Root Cause

Spatial clustering threshold too tight for narrow connections. Footprint-aware merging is the cleanest fix because it uses the authoritative building boundary as the merging criterion.

## Expected Realistic Gain

+20-29 pts with footprint-aware merging. **This is the single highest ROI fix in the entire backlog.**
