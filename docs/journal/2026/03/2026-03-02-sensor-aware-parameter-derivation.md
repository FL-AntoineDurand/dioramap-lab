---
id: "2026-03-02-sensor-aware-parameter-derivation"
date: 2026-03-02
title: "Sensor-Aware Parameter Derivation: All Thresholds From Physics"
tags: [sensor-params, density, z-noise, derivation, architecture]
category: development
status: concluded
refs:
  - "2026-03-02-extract-denoise-lazy-init-decoupling"
  - "2026-03-02-ign-lidar-hd-source-characterization"
pipeline: v6a-base
design_doc: "docs/ARCHITECTURE.md"
---

## Context

Pipeline algorithm parameters (distance thresholds, bin sizes, minimum point counts, KDE bandwidths) were hardcoded constants tuned for IGN LiDAR HD at ~10 pts/m². This made the pipeline fragile when applied to denser surveys (30 pts/m²) or sparser legacy data (5 pts/m²), and hid the physical reasoning behind each value.

## Approach

Created a central derivation module (`lib/pipeline/sensor-params.js`) with 7 functions that derive all algorithm thresholds from two physical sensor characteristics:

- **Point density** (ρ, pts/m²) — drives neighborhood sizes, minimum point counts
- **Z-noise** (σ_z, meters) — drives distance thresholds, bin sizes, KDE bandwidths

Key design decisions:

1. **Physics-first, not backward-compatible**: Formulas are derived from geometry and noise theory, NOT anchored to "what the old default was". Example: `distThreshold = 4.5 × σ_z` (from 4.5σ confidence interval), not "0.135 because that's what 3σ × 0.03 was".

2. **Tilt-dependent density**: `effectiveDensity = nadirDensity × cos(maxTilt)`. A 45° roof gets 71% of flat-roof point density, which directly affects minimum detectable area.

3. **Every derived value becomes an artifact** with a description explaining the formula. This is critical for UI visualization — users can inspect why a threshold was chosen.

4. **Override pattern**: `params.X ?? derivedValue` — explicit pipeline params always win over sensor-derived defaults. Convention: `default: null` in meta means "auto-derived from sensor".

## Derivation Formulas

| Function | Formula | Floor | IGN (10/0.03) |
|----------|---------|-------|---------------|
| `deriveKnn(ρ)` | k = π × min(0.5, 4/√ρ)² × ρ | k ≥ 6 | k=16, r=0.50m |
| `deriveMinPeakCount(ρ)` | round(3.0m² × ρ × cos(50°)) | ≥ 3 | 19 pts |
| `deriveMinRegionPts(ρ)` | round(1.0m² × ρ × cos(50°)) | ≥ 3 | 6 pts |
| `deriveDistThreshold(σ_z)` | 4.5 × σ_z | ≥ 0.05m | 0.135m |
| `deriveOffsetBinSize(σ_z)` | 3.0 × σ_z | ≥ 0.03m | 0.09m |
| `deriveOffsetTolerance(σ_z)` | 4.0 × σ_z | ≥ 0.05m | 0.12m |
| `deriveOffsetKdeParams(σ_z)` | bwMin=1.5σ, bwMax=7.0σ | bwMin≥0.02 | 0.045–0.21m |

## Files Modified

11 step files updated (version bumped in each):

| Step | Version | Key Changes |
|------|---------|-------------|
| `extract-denoise` | 3→4 | Publishes `lidar-density`, `lidar-z-noise` |
| `compute-normals` | 2→3 | `normalK` from `deriveKnn(density)` |
| `competitive-grow` | 2→3 | `growK` from `deriveKnn(density)` |
| `point-stealing` | 2→3 | `growK` from `deriveKnn(density)` |
| `asymmetric-steal` | 1→2 | `growK` from `deriveKnn(density)` |
| `normal-cascade-segment` | 3→4 | 10 params derived (bins, peaks, KDE, tolerances) |
| `normal-cascade-seed` | 1→2 | `distThreshold`, `minSeedPts` derived |
| `cascade-ransac-seed` | 1→2 | `distThreshold`, `minSeedPts` derived |
| `ransac-seed` | 2→3 | `distThreshold`, `minInliersFloor` derived |
| `normal-histogram-seed` | 3→4 | `distThreshold`, `minPeakCount`, `minRegionPts` derived |
| `histogram-seed` | 3→4 | `histBinSize`, `spikeBandwidth`, `miniRansacDist` derived |

## Sensor Profiles for Testing

5 synthetic sensor profiles (40 new test cases, 505 total):

| Profile | Density | σ_z | XY Offset | Misclass |
|---------|---------|-----|-----------|----------|
| ign-roof | 10 | 0.03m | 0.40m | 5% |
| ign-clean | 10 | 0.02m | 0.20m | 2% |
| ign-noisy | 10 | 0.05m | 0.50m | 8% |
| dense-survey | 30 | 0.02m | 0.10m | 1% |
| sparse-legacy | 5 | 0.05m | 0.50m | 10% |

## Results

- **1463 tests pass** (0 failures)
- **28 new unit tests** for derivation functions (cross-function consistency verified)
- **Benchmark**: 7/7 quick cases pass, avg score 95.8
- **Sensor profile smoke test**: v6a-base scores 90.3 (sparse-legacy) to 96.1 (all others) — pipeline adapts correctly to different sensor characteristics

## Key Insight: peakMinFrac Was Wrong

The old `peakMinFrac = 5%` threshold was relative to building size. For a 15,000-point building, the minimum peak was 766 points — too high to detect a small church tower annex (~100 points). The physics-based approach uses `minDetectableArea × density × cos(maxTilt)`, giving a fixed 19-point threshold at IGN density regardless of building size.
