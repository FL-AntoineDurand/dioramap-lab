---
id: "2026-02-25-v3-final-benchmarks"
date: 2026-02-25
title: "V3 Final Benchmarks and Weak Spot Analysis"
tags: [v3, benchmark, weak-spots, parameter-sensitivity]
category: benchmark
status: concluded
refs: ["2026-02-25-overlap-sort-bug", "2026-02-25-v3-development-overnight"]
---

# V3 Final Benchmarks and Weak Spot Analysis

## Results (post-fix)

| Algorithm | V1 (240 synth) | V1 (30 real-proc) |
|-----------|:-:|:-:|
| champion_v2 | **92.0** | 86.8 |
| early_categorized | **92.0** | 87.4 |
| v3_combined | 91.9 | **87.7** |

## Parameter Sensitivity

12 variations on 13 hard cases, max possible improvement +0.07 pts.

## Weak Spots Identified

- Row houses: ~50 (worst case)
- Dormer house: ~79
- Cross plan: ~83
- Flat L-shaped: ~71

Created `V3-ALGORITHM-REPORT.md` (538 lines) and `ALGORITHM-HISTORY.md` (279 lines).
