---
id: "2026-03-02-ign-lidar-hd-source-characterization"
date: 2026-03-02
title: "IGN LiDAR HD source characterization — sensor, accuracy, noise, classification"
tags: [data-source, ign, lidar-hd, accuracy, noise, classification, reference]
category: finding
status: concluded
refs: []
design_doc: "docs/reference/IGN-LIDAR-HD-SPECIFICATIONS.md"
---

# IGN LiDAR HD Source Characterization

## Motivation

All 403 real-building test cases (`real-*`, `real-proc-*`) consume COPC tiles from the IGN LiDAR HD programme. Understanding the source's accuracy, noise, density and classification reliability is essential for interpreting benchmark results and calibrating synthetic test case parameters.

## Key Findings

### Sensor & Acquisition

- **Multi-vendor programme**: 4 consortia using Riegl VQ-1560 II/II-S, VQ-780 II-S, Leica CityMapper-2, and Teledyne Optech sensors. Different 50 km blocks have different noise profiles.
- **Flight altitude**: ~1,200 m AGL, 50% strip overlap, 20 half-angle scan.
- **Contractual minimum**: 10 pulses/m. Actual measured: ~37-40 pts/m (multi-return in vegetation). On hard surfaces (rooftops): ~10-12 pts/m (single return).

### Accuracy

| Metric | Specification | Estimated actual |
|--------|---------------|------------------|
| Z accuracy (RMSE) | < 10 cm | ~5-10 cm |
| XY accuracy | < 50 cm | ~20-50 cm |
| Sensor ranging | - | 3-5 mm |
| GNSS contribution | - | 2-5 cm |
| IMU angular errors | - | 5-15 cm planimetric |

### Noise on Rooftops

**No published noise sigma on flat surfaces exists.** IGN's own internship posting acknowledges quality characterization is "minimal." Estimated from sensor specs and ALS literature:

| Surface | Estimated Z noise (1 sigma) |
|---------|---------------------------|
| Flat roofs | **2-5 cm** |
| Bare ground | 3-7 cm |
| Vegetation | 10-30 cm |

### Classification

11 classes (ASPRS 1-6, 9, 17 + IGN-specific 64-67). Hybrid pipeline: TerraScan (rules) + Myria3D (RandLA-Net deep learning) + lidar-prod (BD Topo cross-reference).

Building class 6 performance (Myria3D model level):
- **Precision**: 96.2%
- **Recall**: 93.7%
- **IoU**: 90.4%
- **Production pipeline** (with BD Topo): >98% precision/recall on point groups

~2-6% of actual building points are misclassified, mainly at edges and overhanging vegetation.

### Format

COPC 1.0 (LAZ 1.4), PDRF 6 (no RGB), 1 km tiles, Lambert-93 + NGF-IGN69 orthometric heights. HTTP range requests supported for streaming extraction.

## Implications for Benchmark

1. **Density**: Real rooftop density (~10-12 pts/m) is closest to our `sparse` synthetic variant (10 pts/m). Our `standard` (30 pts/m) is a 3x overestimate.
2. **Noise**: Real noise (~2-5 cm on flat roofs) falls in our `clean` (1 cm) to `standard` (3 cm) range. Our `standard` is a realistic match.
3. **XY offset**: 30-50 cm planimetric accuracy explains the BDTOPO footprint misalignment we observe, motivating the co-registration step.
4. **Multi-sensor variability**: Benchmark should ideally include test cases from multiple acquisition blocks to capture inter-sensor differences.
5. **Edge misclassification**: The 6% building recall gap at model level occurs precisely at roof boundaries where plane segmentation accuracy matters most.

## Reference Document

Full 16-section specification compiled in [`docs/reference/IGN-LIDAR-HD-SPECIFICATIONS.md`](../reference/IGN-LIDAR-HD-SPECIFICATIONS.md) covering sensor hardware, flight parameters, point density, accuracy budget, noise estimates, full classification code table, Myria3D per-class metrics, COPC format details, coordinate system, data access endpoints, coverage timeline, comparison with AHN4/3DEP/EA, and known limitations.

## Sources

- FRACTAL paper (arXiv:2405.04634) — 250 km test set, per-class accuracy metrics
- LIDAR Magazine (May 2024) — production pipeline description, automation rates
- IGN FAQ — contractual specs, altitude exceptions, classification codes
- Riegl VQ-1560 II-S datasheet — sensor ranging accuracy, scan parameters
- IGN internship posting — acknowledgement of quality characterization gaps
