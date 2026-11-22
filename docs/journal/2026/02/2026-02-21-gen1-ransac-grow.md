---
id: "2026-02-21-gen1-ransac-grow"
date: 2026-02-21
title: "Gen 1: RANSAC Grow — Competitive Point Stealing Breakthrough"
tags: [gen1, ransac-grow, competitive-growing, point-stealing, breakthrough]
category: development
status: concluded
milestone: "Competitive growing breakthrough"
metrics:
  score: 83
  test_suite: "80 synth"
---

# Gen 1: RANSAC Grow -- Competitive Point Stealing Breakthrough

`ransac-grow.js` -- competitive region growing with point stealing (0.80 ratio threshold), ~83.

## Breakthrough: Competitive Growing

After RANSAC finds initial planes, each plane's boundary is expanded via k-NN frontier. When multiple planes compete for the same point, the one with the lowest point-to-plane distance wins (at 0.80 ratio threshold).

This single innovation jumped scores from ~80 to ~83 and became the core of every subsequent algorithm. Competitive growing for boundary refinement beats all Gen 0 approaches.
