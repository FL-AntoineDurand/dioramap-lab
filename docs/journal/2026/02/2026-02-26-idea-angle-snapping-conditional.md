---
id: "2026-02-26-idea-angle-snapping-conditional"
date: 2026-02-26
title: "Idea: Angle Snapping (Conditional, with BDTOPO Priors)"
tags: [idea, angle-snapping, regularization, bdtopo, retry]
category: idea
status: abandoned
---

# Idea: Angle Snapping (Conditional, with BDTOPO Priors)

## Original Attempt
Track A1 (`reg_a1_pairwise_snap`) — snap adjacent plane dihedral angles to 0/30/45/60/90 degrees.

## Why It Failed
+0.0 on 305-case. Plane equations from RANSAC+LS are already accurate.

## Why Retry
On real buildings (not synthetic), plane equations can be noisy. Gate the snapping behind BDTOPO metadata: only snap when `nature != 'En ruine'` and `materiaux_de_la_toiture in ('1', '2')` and deviation < 2 degrees.

## How to Retry
Optional flag in `refit-planes.js`: `snap: true, snapThreshold: 2.0, snapAngles: [0, 15, 30, 45, 60, 90]`.

## Expected Impact
+0.1-0.3 pts on real buildings. Main value is visual quality for solar panel layout.

## Difficulty
Easy.
