---
id: "2026-02-26-idea-ridge-detection-validation"
date: 2026-02-26
title: "Idea: Ridge Detection as Validation (Not Primary Seed)"
tags: [idea, ridge, hessian, validation, retry]
category: idea
status: abandoned
refs: ["2026-02-22-ridge-first-detection"]
---

# Idea: Ridge Detection as Validation (Not Primary Seed)

## Original Attempt
`ridge_detect.js` (Gen 1) — Hessian eigendecomposition + flood fill. Used ridge lines as the PRIMARY seed to divide the roof into regions.

## Why It Failed
Catastrophic failure on hip roofs (44.8). Ridges don't form complete barriers between hip planes.

## Why Retry
As a VALIDATION step (not a seed), ridge detection could confirm or reject plane boundaries after RANSAC/normal-histogram seeding. Strong ridges matching plane intersection lines = high confidence. Plane intersections with no matching ridge = possible false split.

## How to Retry
New step `ridge-validate.js`. Input: detected planes + LiDAR points. Compute Hessian eigenvalues along plane intersection lines. Score each intersection: `ridge_strength = mean(lambda2 / lambda1)`. Low strength = merge candidate.

## Expected Impact
+2-3 pts on `cross_plan` (reduce false splits at valley intersections).

## Difficulty
Medium.
