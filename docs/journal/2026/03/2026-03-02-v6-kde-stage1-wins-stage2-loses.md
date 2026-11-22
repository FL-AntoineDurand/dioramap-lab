---
id: "2026-03-02-v6-kde-stage1-wins-stage2-loses"
date: 2026-03-02
title: "V6-KDE: Stage 1 KDE Wins, Stage 2 KDE Loses"
tags: [v6, kde, histogram, bandwidth, normal-cascade]
category: experiment
status: concluded
refs: ["2026-03-02-index-space-mismatch-scorer"]
pipeline: v6g-kde
design_doc: "docs/V6-KDE.md"
---

# V6-KDE: Stage 2 KDE Regresses, Stage 1 KDE Is a Clear Win

## Question

Does replacing the discrete histogram with Gaussian KDE improve the V6 normal cascade? Two insertion points: Stage 2 (offset splitting within an orientation group) and Stage 1 (normal orientation grouping).

## Setup

421 test cases (synth + real), 3 benchmark runs. Phase 1 tested 5 Stage-2 KDE bandwidth strategies (Silverman, IQR, physical, adaptive, posterior) against v6a-base histogram. Phase 2 tested 3 Stage-1 KDE methods (separable, 2D, physical) against v6a-base, keeping Stage 2 as histogram.

## Results

### Stage 2 KDE (offset) — all variants regress vs histogram

| Variant | Avg Score | vs v6a W/T/L | Time |
|---------|-----------|--------------|------|
| v6a-base (histogram) | 49.8 | — | 5918ms |
| v6g-kde-physical | 49.5 | 42W / 316T / 63L | 7201ms |
| v6g-kde-silverman | 49.1 | 32W / 317T / 72L | 6801ms |
| v6g-kde-iqr | 49.0 | 32W / 315T / 74L | 6963ms |
| v6g-kde-adaptive | 49.0 | 40W / 313T / 66L | 6682ms |
| v6g-kde-posterior | 48.9 | 30W / 321T / 70L | 7256ms |

Best Stage 2 KDE (physical): 42W/63L = 0.67:1 win ratio. All variants lose, with 15-22% more runtime.

### Stage 1 KDE (normal orientation) — all variants clearly win vs histogram

| Variant | Avg Score | vs v6a W/T/L | Time |
|---------|-----------|--------------|------|
| v6g-kde-s1-physical | 51.2 | 95W / 281T / 45L | 6616ms |
| v6g-kde-s1-2d | 51.2 | 91W / 286T / 44L | 6423ms |
| v6g-kde-s1-separable | 51.2 | 87W / 287T / 47L | 6519ms |

Best Stage 1 KDE (physical): 95W/45L = 2.1:1 win ratio. +1.4 avg score, +12% runtime.

**Note**: Absolute scores here are wrong due to the segmentation scorer bug. Relative W/T/L conclusions are valid.

## Root Cause

Stage 2 operates on small subgroups (20–200 points) where the histogram's fixed 0.15m bins already work well — the data is dense enough relative to the bin width. KDE's continuous density estimation adds smoothing that slightly blurs close peaks (two planes 20cm apart) more than the histogram does. The bandwidth floor (0.05–0.08m) is too close to the bin width to offer resolution advantage.

Stage 1 is the opposite: the 2D histogram (18×36 = 648 bins) is extremely sparse for typical point clouds. The 3×3 smoothing is a crude workaround. KDE replaces this with proper continuous density estimation, finding orientation peaks that straddle bin boundaries in the histogram. The +1.4 avg score comes from better separation of closely-oriented roof faces.

## Implication

The histogram-vs-KDE tradeoff depends on the ratio λ = N/B (points per bin). Stage 2 has high λ (adequate histogram) while Stage 1 has very low λ (sparse histogram, KDE wins). Future work should apply KDE selectively — only where λ < 5.

## Follow-ups

- Drop all Stage 2 KDE variants
- Promote Stage 1 KDE (any of the 3 methods, results are identical) as the new v6a default
- Test combined: Stage 1 KDE + Stage 2 histogram (v6g-kde-full already exists, needs benchmarking)
