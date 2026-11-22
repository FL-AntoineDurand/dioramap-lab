---
id: "2026-02-22-ransac-grow-clip-reseed"
date: 2026-02-22
title: "RANSAC Grow + Clip + Reseed: Iterative Reseeding"
tags: [gen1, reseed, iterative]
category: development
status: concluded
milestone: "Iterative reseeding"
metrics:
  score: 98.7
  test_suite: "80 synth"
---

# RANSAC Grow + Clip + Reseed: Iterative Reseeding

`ransac-grow-clip-reseed.js` (~633 lines) -- iterative RANSAC reseeding from unassigned points, `maxReseedIter=3`.

## Key Improvement

Mansard roof type saw the largest gain: 91.1 to 97.0 (+5.9). The `upper_west` plane, previously missed by single-pass RANSAC, was rescued by the reseed pass finding it among the remaining unassigned points.

## Results

Average: 98.7 on 80 cases (+0.6 vs baseline).

## Milestone

Committed project to git, wrote README.
