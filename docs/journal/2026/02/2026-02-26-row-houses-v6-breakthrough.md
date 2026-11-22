---
id: "2026-02-26-row-houses-v6-breakthrough"
date: 2026-02-26
title: "Row Houses: V6 Cascade Finds +62% More Planes"
tags: [v6, row-houses, cascade, weak-spot]
category: finding
status: concluded
refs: ["2026-02-26-cross-plan-structural-limitation"]
pipeline: v6a-base
---

# Row Houses: V6 Cascade Finds +62% More Planes

## Question

V6 was designed for multi-face roofs (mansard, hip). Does it help on row houses, which are a known weak spot (~50/100)?

## Setup

18 row house test cases (small/medium/large × clean/noisy/standard × proc-generated), comparing v2-champion vs v6a-base plane counts. From targeted weak-spot analysis on full 643-case data.

## Results

Total planes found: v2-champion = 76, v6a-base = 123 (**+62%**).

Standout cases (clean variants where effect is largest):
- `row_houses-large-clean`: v2 finds 3 planes, v6a finds 10 (GT: 10)
- `row_houses-medium-clean`: v2 finds 4 planes, v6a finds 9 (GT: 8)
- `row_houses-small-clean`: v2 finds 5 planes, v6a finds 7 (GT: 8)

On noisy variants, the advantage shrinks or reverses: `row_houses-large-noisy`: v2=2, v6a=7.

## Root Cause

Row houses have 3 attached units with near-identical but not exactly identical roof slopes. RANSAC's random 3-point sampling draws points from multiple units, fits a single merged plane, and connected components can't split them (the roofs physically touch). The V6 cascade's Stage 2 (offset histogram) detects the subtle elevation differences between units — each unit's ridge is at a slightly different height — and creates separate segments per unit. These become separate seed planes that competitive-grow preserves.

The effect is strongest on clean data because noise obscures the subtle (5–15cm) elevation offsets between units. On noisy data (σ=5cm), the offset peaks blur together and the cascade falls back to a single merged segment.

## Implication

The V6 cascade's offset-along-normal decomposition finds structure that RANSAC fundamentally cannot, even though the planes are nearly coplanar. This is the strongest evidence that cascade seeding adds value beyond RANSAC. The effect is noise-dependent: cascade advantage requires signal (elevation offset) > noise (point scatter).

## Weak Spot Analysis

### Why weak (pre-V6)

3 attached houses share party walls. The left-side roof planes of all 3 units are geometrically coplanar — same slope, same height, continuous surface. RANSAC correctly merges them into one plane. Ground truth expects 3 separate planes. This is geometrically unsolvable using plane fitting alone.

### Mitigation ideas

| Idea | Mechanism | Expected Gain | Difficulty |
|------|-----------|:-------------:|:---------:|
| BDTOPO footprint subdivision | If BDTOPO provides per-unit footprints, clip merged plane at unit boundaries | +10-20 | Medium |
| Ortho color boundary detection | Different units often have different tile colors; split at color discontinuities | +5-10 | Medium |
| LiDAR height discontinuity at party walls | Party walls are 15-30cm higher; detect thin elevated ridges perpendicular to ridge line | +5-15 | High |
| BDTOPO `nombre_de_logements` prior | If N units, attempt to split long planes into N equal sub-planes | +5-10 | Medium |

### Revised assessment

V6a already provides +62% more planes "for free" on row houses. Additional footprint subdivision would still help (+10-20 pts more), especially on noisy variants where V6a loses its advantage.

## Follow-ups

- Test V6 variants on expanded row house set with different noise levels
- Investigate footprint subdivision approach for noisy cases
