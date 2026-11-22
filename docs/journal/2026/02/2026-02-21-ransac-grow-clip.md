---
id: "2026-02-21-ransac-grow-clip"
date: 2026-02-21
title: "RANSAC Grow + Clip: Parallel-Edge Hull Clipping at Ridges"
tags: [gen1, hull-clipping, ridge, parallel-edge]
category: development
status: concluded
milestone: "Parallel-edge clip"
metrics:
  score: 85
  test_suite: "80 synth"
---

# RANSAC Grow + Clip: Parallel-Edge Hull Clipping at Ridges

`ransac-grow-clip.js` -- parallel-edge hull clipping at ridge lines (20deg tolerance), ~85.

## Results

Major gains across roof types:

- Gable: +3
- Hip: +4
- Multi-section: +5

## Reusable Output

Exported 5 clip helper functions for reuse in subsequent algorithms.
