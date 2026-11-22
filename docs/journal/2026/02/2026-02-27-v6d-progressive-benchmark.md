---
id: "2026-02-27-v6d-progressive-benchmark"
date: 2026-02-27
title: "V6d Progressive Full 643-Case Benchmark: 203W/46L (4.4:1)"
tags: [v6d, progressive, benchmark, 643-case]
category: benchmark
status: concluded
refs: ["2026-02-26-v6-643-case-benchmark"]
pipeline: v6d-progressive
metrics:
  wins: 203
  ties: 394
  losses: 46
  dataset: "643 all (pre-fix)"
---

# V6d Progressive Full 643-Case Benchmark: 203W/46L (4.4:1)

## Results

- **203W 394T 46L** vs v2-champion (4.4:1 win rate). Avg 8109ms (+123% vs v2).
- Ran on OLD test data (before footprint fixes) — 7 errors, 13 degraded cases.

## Key Categories

- split_level 14W/0L, sawtooth 8W/0L, church 10W/0L, silo 14W/2L

## New Regressions vs v6a

- cross_plan 15L (vs 12L for v6a)
- row_houses 4L (vs 0L for v6a)
- multi_section 7L

## Conclusion

v6d is the max-detection variant (+54% wins vs v6a) but 2.2x slower.
