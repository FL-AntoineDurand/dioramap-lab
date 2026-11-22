---
id: "2026-02-26-idea-asymmetric-flat-stealing"
date: 2026-02-26
title: "Idea: Asymmetric Stealing for Flat Roofs"
tags: [idea, flat, stealing, coplanar, merge]
category: idea
status: proposed
---

# Idea: Asymmetric Stealing for Flat Roofs

## What
After categorization, allow roof planes to steal points from obstacle planes but block the reverse. Additionally, for flat roofs specifically: allow the largest flat plane to steal from smaller coplanar flat fragments.

## Why
On flat roofs with obstacles, parapets "win" edge points during competitive growing, shrinking the main roof hull. For flat L-shaped buildings, the two arms could be merged by letting the larger fragment absorb the smaller one.

## How
Already partially implemented in `asymmetric-steal.js`. Extend it with a flat-merge mode: if two planes are coplanar (normal angle < 2 deg, d-value diff < 0.1m), the larger plane steals from the smaller one regardless of spatial connectivity.

## Expected Impact
+5-10 pts on `flat_l_shaped` (71 → 80+). Small improvement on `flat_with_*` obstacle types.

## Difficulty
Easy (extend existing step).
