---
id: "2026-02-27-bad-footprints-silent-corruption"
date: 2026-02-27
title: "20 Bad Footprints Silently Corrupt Benchmark Results"
tags: [footprints, test-data, benchmark, data-quality]
category: bugfix
status: concluded
---

# 20 Bad Footprints Silently Corrupt Benchmark Results

## Question

Why do 7 real test cases crash and 13 others show suspiciously low plane counts for both v2 and v6a?

## Setup

Full 643-case benchmark (run 1). Investigated crash cases (0 planes, null artifact errors) and cases where both pipelines found <3 planes on buildings expected to have 5+.

## Results

7 crash cases: footprint area was 2–16 m² for buildings that are 200–850 m² in reality. The tiny footprint extracted 0 LiDAR points → null artifact → crash in downstream steps.

13 silent degradation cases: footprint area was 1–570 m² for 200–2,000 m² buildings. Partial point extraction produced 3–50% of expected points. Both v2 and v6a were equally degraded.

Without bad footprint (set to null, use all building-class points): v2 gains avg +6.3 planes/case, v6a gains avg +5.5 planes/case. The original "fair" comparison (7W 6T 0L for v6a) becomes 8W 0T 5L — v2 benefits slightly more from full point clouds.

All 20 cases were `real-*` (imported from production API), not `real-proc-*` (procedural). Root cause: the test case generation script (`import-real-building.js`) had a footprint extraction bug for large buildings where the WFS bounding box query returned an adjacent smaller building's footprint.

## Fix

Set footprint to `null` on all 20 cases (original saved as `bad_footprint`). Updated headline benchmark numbers: 132W 473T 38L (was 128W 484T 31L before fixes).

## Implication

Bad input data can silently degrade benchmark results without any visible error. The footprint was technically valid GeoJSON — just for the wrong building. Crash cases are easy to detect; silent degradation cases are not. Any benchmark on real data should include a sanity check: `footprint_area / expected_building_area` ratio, flagging cases where the footprint captures <50% of the expected building envelope.

## Pipeline Robustness Fixes

- Added null guards in extract-denoise, compute-normals, normal-cascade-segment (prevent crashes on 0-point extraction)
- Added runner pre-check for `until` condition (skip repeat blocks when already done)
- Consider adding a footprint-vs-pointcloud coverage check as a pipeline validation step
