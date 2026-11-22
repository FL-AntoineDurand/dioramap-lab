---
id: "2026-03-02-unfold-ransac-ridge-bitmask"
date: 2026-03-02
title: "Unfold RANSAC Loop + Unified Ridge Bitmask"
tags: [ransac, ridge, bitmask, pipeline, refactor]
category: development
status: concluded
refs: ["2026-02-26-v6-normal-cascade-implementation", "2026-03-02-cascade-step-split"]
---

## Key Findings

### RANSAC Loop Never Needs Iteration 2+

Benchmark on 42 real buildings showed RANSAC mop-up (after cascade seeding) finds new
seeds in ~50% of cases on iteration 1, but **never** finds additional seeds on iterations
2 or 3. The repeat loop `{ repeat: [ransac, grow, steal, refit], max: 3, until: noNewSeeds }`
was pure overhead — executing the "until" check and loop control without ever triggering
a second pass.

**Decision**: Unfold to flat sequence: `grow → ransac → grow → steal → refit`. This:
- Saves 2 redundant steps per pipeline execution (the initial steal + refit before ransac)
- Removes all loop control infrastructure (`noNewSeeds`, `outerIter`, `controlState`,
  `distSchedule`, `inlierSchedule`, `growSchedule`)
- Simplifies 20 pipelines (all had repeat blocks)
- v2 family (no cascade) simplifies to `ransac → grow → steal → refit` (single pass)

### No Duplicate Pipelines After Unfolding

Verified programmatically: no two pipelines became identical after unfolding. All
same-step groups already existed and differ in their defaults.

### Ridge Information Was Being Discarded

`competitive-grow` and `point-stealing` detect boundary points internally (via k-NN
neighbor checks) but never recorded this information. Meanwhile, the cascade ridge
detection produced a simple 0/1/2 array (clean/ridge/orphan) that couldn't distinguish
which detection method flagged each point.

**Decision**: Unified bitmask artifact `ridge-candidates` (Uint8Array) with per-source
bits:

| Bit | Value | Source | Step |
|-----|-------|--------|------|
| 0 | 1 | cascade normal-space | cascade-ridge-detect |
| 1 | 2 | cascade offset-space | cascade-ridge-detect |
| 2 | 4 | cascade k-NN | cascade-ridge-detect |
| 3 | 8 | grow boundary | competitive-grow |
| 4 | 16 | steal boundary | point-stealing |
| 7 | 128 | orphan | cascade-shared (preliminary) |

Each step ORs its bits in. Downstream consumers check `value & 0x7F` (any ridge) or
`value & 128` (orphan). The UI renders by popcount: yellow (1 source), orange (2),
red (3+), purple (orphan).

## Changes

### Pipeline (20 pipelines)
- All repeat blocks replaced with flat sequences
- `maxStealIter`, `distSchedule`, `inlierSchedule`, `growSchedule` removed from defaults

### Steps (version bumps)
- `ransac-seed` v3→v4: removed loop control, single-pass only
- `competitive-grow` v4→v5: removed schedule, added boundary scan → OR bit 3
- `point-stealing` v4→v5: removed iteration cap, added boundary scan → OR bit 4
- `cascade-ridge-detect` v1→v2: bitmask OR instead of 0/1/2 values
- `normal-cascade-seed` v4→v5: reads `ridge-candidates` (was `cascade-ridge-candidates`),
  exclusion check uses `& 0x7F`
- `extract-denoise`: removed `noNewSeeds` control
- `histogram-seed`: removed `noNewSeeds` control

### UI
- `ridge-candidates`: popcount-based gradient (yellow→orange→red) + purple orphans
- `cascade-ridge-*` debug artifacts: simple dim/red + description noting `ridge-candidates`
  is the main aggregate
- `owner-post-grow`: frozen snapshot after competitive-grow (categorical coloring)
- `owner-post-steal`: frozen snapshot after point-stealing (categorical coloring)

> **Note** (2026-03-04): `owner-post-grow` and `owner-post-steal` snapshots were removed in the qualified-names refactor. Use `competitive-grow#0.owner` and `point-stealing#0.owner` instead.

### Runner + step occurrence tracking
- Pipeline runner now tracks step occurrence counts: when the same step runs twice
  (e.g. `competitive-grow` before and after `ransac-seed`), the second gets labeled
  `competitive-grow#1` in `stepTimings` and artifact `sourceStep` (0-indexed, no space).
- Artifacts are stored under qualified names: `competitive-grow#0.owner`, `competitive-grow#1.owner`.
- UI strips `#N` suffix for step manifest lookup — both occurrences show as separate
  sections with correct params, timing, and artifacts.

## Verification

- 1535/1535 tests pass
- v6g-kde-full quick benchmark: 7/7, avg 95.9 (no regression)
- v2-champion quick benchmark: 7/7, avg 95.7 (no regression)
- Stale pipeline cache required clearing after version bumps (expected)
