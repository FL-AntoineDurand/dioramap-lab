---
id: "2026-03-02-sensor-derived-defaults-cleanup"
date: 2026-03-02
title: "Remove hardcoded sensor-derived params from all pipeline defaults"
tags: [sensor-aware, pipelines, cleanup, ui]
category: development
status: concluded
refs: ["2026-03-02-sensor-aware-parameter-derivation"]
---

## Context

After implementing sensor-aware parameter derivation (7 functions in `sensor-params.js`), all 22 pipelines still hardcoded the very params that should be derived. The hardcoded values always won via `params.X || derivedValue`, defeating the physics-based formulas.

Concrete impact: on `proc-church-large-standard`, `normalPeakMinFrac: 0.05` produced `minCount=767`, preventing detection of small tower faces (each has <767 pts). Sensor-derived value would be 57 pts.

## Changes

### 1. Removed sensor-derived params from all 22 pipeline defaults

Params removed from every pipeline's `defaults` object in `pipelines.js`:

| Category | Params removed |
|----------|---------------|
| Density-derived | `normalK`, `growK`, `minRegionPts`, `normalPeakMinFrac`, `cascadeOffsetPeakMinFrac`, `cascadeMinSegmentPts`, `cascadeMinSeedPts`, `cascadeOffsetKdeMinPeakPts`, `peakMinFraction` |
| Z-noise-derived | `distThreshold`, `cascadeOffsetBinSize`, `cascadeOffsetTolerance`, `cascadeOffsetKdeBwMin`, `cascadeOffsetKdeBwMax`, `cascadeOffsetKdeNoiseStd`, `histBinSize`, `spikeBandwidth`, `miniRansacDist` |

Non-sensor params (modes, flags, thresholds) kept as-is.

### 2. Removed `v6g-kde-s1-separable` pipeline

After cleanup, `v6g-kde-s1-separable` became identical to `v6g-kde-full` (same steps, same remaining defaults). Removed from registry, updated docs/genealogy. **21 pipelines** now.

### 3. Added `derived` field to step meta.params

All 10 step files with sensor-derived params now have `derived: 'density'` or `derived: 'zNoise'` on each auto-derived param entry (26 params total). This is display metadata only.

### 4. UI: derived param styling

Three visual states for params in step detail view:
- **Orange** — pipeline override (explicit value in defaults)
- **Yellow** — step default (hardcoded fallback)
- **Blue italic** — sensor-derived (`auto (density)` or `auto (zNoise)`)

## Results

- **Church benchmark**: v6g-kde-full now detects 5 planes (was 2), score 89.5
- **Regression check**: 7/7 validation cases pass, avg 95.5 (no change)
- **Tests**: 1489/1489 pass
- No version bumps needed — versions were already bumped when derivation code was added, and cache keys include param signatures

## Cache invalidation

No explicit version bumps required. Cache keys include `stepVersion + stepParams`. Removing a default from the pipeline changes the resolved params signature, which automatically invalidates all cached results for that pipeline.
