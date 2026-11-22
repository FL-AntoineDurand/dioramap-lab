---
id: "2026-02-26-cross-plan-structural-limitation"
date: 2026-02-26
title: "Cross-Plan Buildings: Structural Limitation of Normal-Based Seeding"
tags: [v6, cross-plan, normal-cascade, weak-spot, structural-limitation]
category: finding
status: concluded
refs: ["2026-02-26-row-houses-v6-breakthrough"]
---

# Cross-Plan Buildings: Structural Limitation of Normal-Based Seeding

## Question

Why do all V6 variants consistently lose on cross-plan (L, T, +) buildings?

## Setup

18 cross-plan test cases × 7 V6 variants (a–f + x-hybrid), targeted benchmark. Cross-plan buildings have two rectangular wings joined at right angles, with hip roofs on each wing meeting in a complex valley.

## Results

All V6 variants produce 76–77 total planes across 18 cases. V2-champion produces 100 planes. 12 of v6a's 38 total losses (on 643 cases) come from cross-plan buildings. No V6 variant helps — the regression is identical across a/b/c/d/e/f.

## Root Cause

Each wing's roof has planes at the same tilt but different azimuths. That part works — the cascade separates them. But where the two wings meet, the valley creates coplanar planes (same normal, same offset) in the same spatial cluster. Stage 1 (orientation) groups them. Stage 2 (offset) doesn't separate them (same d-value). Stage 3 (XZ clustering) merges them (connected). Result: two physically distinct hip-roof faces become one over-sized merged plane.

RANSAC handles this by accident: random 3-point sampling occasionally picks points entirely from one side of the valley, connected components then split at the narrow valley connection. This "stochastic separation" is unreliable but works often enough to produce higher plane counts on average.

## Implication

Normal-based seeding cannot separate geometrically identical planes (same normal, same offset, spatially connected). This is a fundamental limitation shared by V4 (normal histogram) and V6 (normal cascade). The fix requires non-geometric information: footprint decomposition into convex sub-regions (use wing boundaries as clustering barriers) or ortho-based color/texture boundaries.

## Weak Spot Analysis

### Mitigation ideas

| Idea | Mechanism | Expected Gain | Difficulty |
|------|-----------|:-------------:|:---------:|
| Footprint-guided spatial clustering | Decompose footprint into convex sub-regions, use as Stage 3 barriers | +10-15 | Medium |
| Valley-aware growth | Freeze frontier points equidistant to 3+ planes; assign after convergence | +5-10 | Medium |
| Ridge validation | Hessian eigenvalue test along plane intersections; merge where no real ridge exists | +2-3 | Medium |
| Ortho ridge confirmation | Only accept plane boundaries that have ortho edge support | +1-3 | Medium |
| Post-merge pass | Merge plane pairs sharing long boundary if angle < 5 deg and d-diff < 0.10m | +3-5 | Low |

### V6 643-case evidence

12 of v6a's 31 losses vs v2 are cross-plan buildings. All V6 variants (a-f) produce the same merged result, confirming the limitation is structural.

### Recommended fix

Footprint-guided spatial clustering (decompose footprint polygon into convex sub-regions, use sub-region boundaries as barriers in XZ flood fill). Two coplanar points in different convex sub-regions → separate segments. Fixes cross-plan without affecting simple rectangles.

## Follow-ups

- Do not attempt more V6 variants to fix this — the limitation is structural
- Footprint-guided spatial clustering is the most promising fix — estimated +10-15 pts on cross-plan
