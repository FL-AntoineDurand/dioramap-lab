---
id: "2026-02-26-idea-parameter-auto-tuning"
date: 2026-02-26
title: "Idea: Parameter Auto-Tuning Per Roof Type"
tags: [idea, parameters, auto-tuning, classification]
category: idea
status: proposed
---

# Idea: Parameter Auto-Tuning Per Roof Type

## What
After an initial fast pass (reduced RANSAC iterations), classify the roof type from the detected planes, then re-run with type-optimized parameters.

## Why
Parameter sensitivity analysis (V3 report) showed the hard cases are limited by algorithmic design, not parameters. But a quick classification pass could identify the roof type and select the best-known parameter set.

## How
New step `classify-and-rerun.js`:
1. Run a fast seed pass (100 RANSAC iterations instead of 3000)
2. Classify: 0-1 flat planes = flat; 2 symmetric planes = gable; 4+ = hip/complex; many small = sawtooth
3. Select parameter profile
4. Re-run full pipeline with selected parameters

## Expected Impact
+0.5-1 pt overall. Main value is robustness.

## Difficulty
Medium. Requires the runner to support mid-pipeline parameter overrides.
