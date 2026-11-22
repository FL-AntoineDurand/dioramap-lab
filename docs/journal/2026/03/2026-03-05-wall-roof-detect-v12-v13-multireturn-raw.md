---
id: "2026-03-05-wall-roof-detect-v12-v13-multireturn-raw"
date: 2026-03-05
title: "wall-roof-detect v12-v13: Multi-Return Bitmask + Raw Point Cloud Bypass"
tags: [pipeline, wall-roof, per-point-metadata, edges, extract-denoise]
category: development
status: concluded
refs: ["2026-03-03-wall-roof-detect-v10-two-phase", "2026-03-03-wall-roof-detect-evolution", "2026-03-04-per-point-metadata-pipeline"]
pipeline: v6g-kde-full
---

## v12: Multi-Return Bitmask + Classification Sub-Typing

### Problem

wall-roof-detect v10 used a binary 0/1 `wall-roof-candidates` artifact and had no
way to exploit per-point LiDAR metadata (ReturnNumber, NumberOfReturns, Classification).

### Design

Converted `wall-roof-candidates` from binary to a **bitmask** (like `ridge-candidates`):

| Bit | Value | Meaning |
|-----|-------|---------|
| 0 | 1 | Height gap detected (base flag) |
| 1 | 2 | Lower side has ground (class 2) = eave |
| 2 | 4 | Lower side is all building (class 6) = dormer / roof step |
| 3 | 8 | Lower side has non-ground non-building = obstacle boundary |
| 4 | 16 | Multi-return: ReturnNumber < NumberOfReturns = physical beam split |

**Classification sub-typing** (bits 1-3): For each height-gap candidate, the step
now classifies the lower-side neighbors by their IGN classification code. This
distinguishes eaves (roof-to-ground) from dormers (roof-to-roof) from obstacle
boundaries (roof-to-vegetation/other).

**Multi-return detection** (bit 4): Points where `ReturnNumber < NumberOfReturns`
indicate a physical beam split at a vertical height rupture (eave, dormer, chimney).
Multi-return does NOT detect ridges (both slopes at same height = no beam split).
MR candidates are grid-clustered, PCA-based dirBin computed, then merged into
the candidate pool with boosted weight (default 3.0x) in edge fitting.

**Weighted edge fitting**: `constrainedFit()` converted to weighted linear regression.
Multi-return points pull the fitted edge toward their precise physical position.

### UI

New `wallRoofBitmask` colorMode: green=eave, teal=dormer, magenta=multi-return,
orange=obstacle, yellow=gap-only, red=other.

---

## v13: Raw Point Cloud Bypass (Denoising Audit)

### Problem

3D spatial denoising (radius=0.5m, minN=3) disproportionately removes multi-return
edge points. These points sit at height discontinuities with few 3D neighbors
within a 0.5m sphere. Empirical data:

| Test Case | Raw Pts | Denoised | Removed | MR Detected (raw) |
|-----------|---------|----------|---------|-------------------|
| Paris Haussmann 5e | 6160 | 3341 | 46% | 1226 |
| Bordeaux Bacalan | 3414 | 2796 | 18% | 201 |
| Strasbourg Steep | 7329 | 6389 | 13% | 79 |

97-100% of removed MR points would survive a 2D (XZ-only) denoiser.

### Full Pipeline Audit (22 steps)

| Category | Count | Rationale |
|----------|-------|-----------|
| **DENOISED OK** | 17 | Plane fitting, RANSAC, KDE, normal estimation, hull extraction |
| **INDIFFERENT** | 3 | flat-detect, fetch-metadata, co-register |
| **SHOULD USE RAW** | 2 | wall-roof-detect, obstacle-filter (partial) |

### Solution

wall-roof-detect v13 reads directly from input artifacts (`positions`,
`classifications`, `footprint`, `return-numbers`, `number-of-returns`),
bypassing the denoised `building-points` artifact entirely. It calls
`extractBuildingPoints()` itself to identify which points to test.

No changes to `extract-denoise` — the denoised path remains untouched for
all 17 plane-fitting/segmentation steps that benefit from clean data.

### Wiring

```
[wall-roof-detect#0]: {
  positions: input-lidar, classifications: input-lidar, footprint: input-bdtopo,
  'return-numbers': input-lidar, 'number-of-returns': input-lidar,
  'lidar-density': extract-denoise#0, 'lidar-z-noise': extract-denoise#0,
  owner: <from segmentation>,
}
```

### Key Insight

Denoising is a **per-purpose** decision, not a global one. Plane-fitting steps
need clean data (noise scatters histogram bins, inflates RANSAC outliers).
Boundary detection steps need edge data (the very points that denoising removes).
The solution is not to change the denoiser but to let each step choose its input.
