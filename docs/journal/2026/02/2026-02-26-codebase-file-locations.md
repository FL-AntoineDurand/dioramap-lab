---
id: "2026-02-26-codebase-file-locations"
date: 2026-02-26
title: "Codebase File Location Cross-Reference"
tags: [review, file-locations, navigation, v1, v2]
category: review
status: concluded
---

# Codebase File Location Cross-Reference

All paths relative to ``.

## V1 Repo (`lidar-algos-bench/`)

| Category | Files | Path |
|----------|-------|------|
| Active algorithms (Gen 1-3) | 22 files | `lib/algorithms/*.js` |
| Archived algorithms (Gen 0) | 6 files | `lib/algorithms/archive/*.js` |
| Algorithm registry | 1 file | `lib/algorithms/index.js` |
| Shared utilities | 1 file | `lib/algorithms/_common.js` |
| Geometry | 8 files | `lib/geometry/*.js` |
| Metrics | 7 files | `lib/metrics/*.js` |
| Categorization | 1 file | `lib/categorize/plane-categorizer.js` |
| Post-processing | 4 files | `lib/postprocess/*.js` |
| Smoothing | 2 files | `lib/smoothing/*.js` |
| Solar | 4 files | `lib/solar/*.js` |
| Synthetic | 3 files | `lib/synthetic/*.js` |
| Real buildings | 6 files | `lib/real-buildings/*.js` |
| Image processing | 3 files | `lib/image/*.js` |
| Alignment | 1 file | `lib/geometry/icp-2d.js` |
| Benchmark results | 12 files | `results/*.json` |
| Documentation | 8 files | `docs/*.md` |

## V2 Repo (`lidar-algos-bench-v2/`)

| Category | Files | Path |
|----------|-------|------|
| Pipeline runner | 1 file | `lib/pipeline/runner.js` |
| Pipeline definitions | 1 file | `lib/pipeline/pipelines.js` |
| Pipeline context | 1 file | `lib/pipeline/context.js` |
| Step cache | 1 file | `lib/pipeline/cache.js` |
| Pipeline steps | 26 files | `lib/pipeline/steps/*.js` |
| Algorithm implementations | 3 files | `lib/algorithms/*.js` |
| Documentation | `docs/` | Reference docs + `journal/` entries |

## Third-Party Evaluation (`lidar-third-parties/`)

| Tool | Result | Location |
|------|--------|----------|
| Open3D (Python RANSAC) | 90.0 on 20 cases | `run-open3d.py` |
| City3D (C++ CGAL) | 56.0 on 20 cases | Built from source |
| Roofer (C++ CGAL region-growing) | 92.2 on 20 cases | `roofer_detect.cpp` |
| PolyFit (C++) | Not usable | Needs pre-segmented .vg input |
| **Our champion** | **98.9 on 20 cases** | Beats all tested tools |

## Key Git Commits

| Hash | Date | Message |
|------|------|---------|
| `5c98e9f` | Feb 21 | Gen 0 + framework |
| `635deb5` | Feb 22 | V1 Champion + ridge_detect |
| `9df5584` | Feb 24 | V2 metrics + concave clip + overlap resolve |
| `3e755b6` | Feb 25 | V2 Champion + V3 all tracks + bug fix |
| `b1e3900` | Feb 25 | Pipeline V2 extraction |
| `6e6218b` | Feb 25 | Pipeline V3 |
| `7a75266` | Feb 25 | Pipeline V4 + compute-normals |
| `870de17` | Feb 25 | Co-registration + alignment + ortho + V5 |
