---
id: "2026-02-20-benchmark-framework-creation"
date: 2026-02-20
title: "Benchmark Framework Creation"
tags: [framework, synthetic, metrics, test-cases]
category: development
status: concluded
milestone: "Benchmark framework established"
---

# Benchmark Framework Creation

## Question

How do we systematically evaluate and compare roof plane detection algorithms with reproducible metrics?

## Implementation

Created `lidar-algos-bench/` from scratch (32 files), establishing the foundational benchmark infrastructure:

### Geometry Library
- k-nearest neighbors (k-NN) spatial search
- Principal Component Analysis (PCA) for plane fitting and normal estimation
- Convex hull computation for plane boundary representation
- Least-squares plane fitting with residual analysis
- Spatial clustering (connected components with distance threshold)

### Synthetic Test Case Generator
- 10 parametric roof types: flat, gable, hip, mansard, gambrel, shed, sawtooth, butterfly, multi-section, L-shaped
- Configurable parameters per type: noise level (3 levels), point density, roof pitch, building dimensions
- Ground truth stored with each test case: plane equations, point assignments, boundary polygons

### Metrics Suite
Five metrics for comparing detected planes against ground truth:

| Metric | Weight in V1 Composite | What It Measures |
|--------|:---------------------:|------------------|
| Detection rate | 25% | Fraction of GT planes matched to a detected plane |
| Segmentation F1 | 25% | Point-level precision and recall of plane assignments |
| Boundary IoU | 20% | Overlap between detected and GT convex hulls |
| Plane accuracy | 15% | Normal angle and offset error of matched planes |
| Completeness | 15% | Fraction of building footprint covered by detected planes |

### Web UI
Three.js-based viewer with:
- Algorithm comparison (side-by-side detected vs ground truth)
- Per-step visualization for debugging algorithm internals
- Test case browser with filtering by type and score

### Initial Test Suite
Generated 80 cases: 8 per roof type (10 types), across 3 noise levels (clean, standard, noisy).

## Key Design Decisions

- **Y-up coordinate system**: Points use (X, Y, Z) with Y as the vertical axis, matching Three.js conventions for direct visualization
- **10 pts/m2 density**: Matches IGN LiDAR HD specification, ensuring synthetic data is representative of real input
- **Ground truth stored per test case**: Each `.json` file contains both the point cloud and the GT plane definitions, making tests fully self-contained and reproducible

## Implication

This framework became the basis for all subsequent algorithm development (V1 through V6). The synthetic test suite grew from 80 to 240 cases over the following days, and the metrics evolved into a 7-metric V2 composite. The V2 rewrite (`lidar-algos-bench-v2/`) preserved the same test case format and metric definitions while rebuilding the pipeline architecture.
