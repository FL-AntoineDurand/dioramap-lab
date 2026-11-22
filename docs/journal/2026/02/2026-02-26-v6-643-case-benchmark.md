---
id: "2026-02-26-v6-643-case-benchmark"
date: 2026-02-26
title: "V6 Full 643-Case Benchmark: 132W/38L (3.5:1 Win Rate)"
tags: [v6, benchmark, 643-case, win-loss]
category: benchmark
status: concluded
refs: ["2026-02-26-v6-normal-cascade-implementation", "2026-02-27-bad-footprints-silent-corruption"]
pipeline: v6a-base
metrics:
  wins: 132
  ties: 473
  losses: 38
  dataset: "643 all"
---

# V6 Full 643-Case Benchmark: 132W/38L (3.5:1 Win Rate)

## Results

- Created `scripts/benchmark-v6.js` for focused synthetic comparison
- Full 241-synth benchmark: v6a-base 26W 212T 2L vs v2-champion (240 synthetic)
- Full 643-case benchmark: v6a-base **132W 473T 38L** vs v2-champion (240 synth + 403 real)
  - 3.5:1 win-to-loss ratio; biggest wins on silos (+9p), octagonal towers (+7p), row houses (+7p)
  - Primary regression: cross-plan buildings (~12/38 losses, coplanar merging)
  - 20 real test cases had bad footprints (too small) -> all fixed (see bad-footprints entry)
  - Speed: +20% overhead (4212ms vs 3496ms avg), but V6a faster on 56% of individual cases

## Script Fixes

- Fixed `compare-variants.js`: manifest.json exclusion, dynamic column width, JSON output with pipeline-suffixed filenames
- Fixed `benchmark-v6.js`: manifest.json exclusion, added `--include-real` flag
- Fixed `verify-pipeline.js`: manifest.json exclusion

## Targeted Analysis

### Cross-plan deep dive (18 cases x 7 variants)
All V6 variants produce 76-77 total planes vs V2's 100. Regression is structural (coplanar merging). No variant helps.

### Silo + octagonal tower (36 cases x 7 variants)
V6d-progressive best (280p), V6a close (276p). **V6b-ridge-aware loses 29 planes** (247p) -- ridge exclusion too aggressive on multi-faceted structures. Strongly confirms v6b should be dropped.

### Weak-spot analysis (44 cases, v2/v6a/v6d)
- **Row houses: +62% planes with v6a** (123 vs V2's 76). Cascade detects per-section elevation/orientation differences. Major surprise finding.
- Dormer houses: unchanged (all 2p). Dormers too small for cascade segments.
- Flat L-shaped: unchanged (all 1p). XZ clustering limitation persists.

Created `scripts/analyze-compare.js`: reusable analysis script for compare-variants JSON output.
