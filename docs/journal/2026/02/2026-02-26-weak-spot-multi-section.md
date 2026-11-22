---
id: "2026-02-26-weak-spot-multi-section"
date: 2026-02-26
title: "Weak Spot Analysis: Multi-Section (~85/100, V4/V6 Fundamental Weakness)"
tags: [weak-spot, multi-section, coplanar, normal-histogram, v4, v6]
category: finding
status: concluded
refs: ["2026-02-26-cross-plan-structural-limitation", "2026-02-26-idea-ensemble-methods"]
---

# Weak Spot Analysis: Multi-Section (~85/100, V4/V6 Fundamental Weakness)

## Why Weak

Coplanar, spatially adjacent sections with identical normals AND d-values AND connected XZ regions. Normal histogram seeding (V4) cannot separate them because they appear as a single peak in both normal space and d-value space. RANSAC handles this naturally via random sampling + connected components, which accidentally separates them when sample points fall in different sections.

## Ideas That Could Help

| Idea | Mechanism | Expected Gain | Difficulty |
|------|-----------|:-------------:|:---------:|
| Ensemble | Use V2 (RANSAC) for multi_section, V4 for everything else | +10 | Medium |
| XZ grid subdivision | Divide footprint into 5m grid cells; cluster within each cell, then merge | +5-10 | Medium |
| Ortho color splitting | If adjacent sections have different materials, split at color boundary | +3-5 | Medium |

## Root Cause

Normal histogram fundamentally cannot separate geometrically identical planes. This is a known limitation of the V4 approach and equally affects V6 Normal Cascade (same normal group, same offset peak, same spatial cluster = one segment). V2 (RANSAC + CC) handles it because random sampling naturally breaks spatial ties.

## V6 643-Case Evidence

12 of v6a's 31 losses vs v2 are cross-plan buildings. All V6 variants (a-f) produce the same merged result.

## Footprint-Guided Spatial Clustering

Decompose the footprint polygon into convex sub-regions. Use sub-region boundaries as barriers in the XZ flood fill. Two coplanar points in different convex sub-regions → separate segments. Fixes cross-plan (L, T, +) without affecting simple rectangles. Medium difficulty, +10-15 expected gain.

## Expected Realistic Gain

+10 pts with ensemble approach, or +10-15 with footprint-guided clustering.
