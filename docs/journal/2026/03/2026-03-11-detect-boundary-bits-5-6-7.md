---
id: "2026-03-11-detect-boundary-bits-5-6-7"
date: 2026-03-11
title: "Detect-boundary: ground-prox, wall-return, MR-noise (bits 5-7)"
tags: [detect-boundary, multi-return, lidar-physics, boundary-detection]
category: development
status: concluded
refs: ["2026-03-10-physics-multireturn-edge-classification", "2026-03-09-boundary-co-registration"]
pipeline: v6a-base
---

## Context

The detect-boundary step previously used 5 bits (0-4) to classify boundary
candidate points. This session extended the bitmask with 3 new bits to capture
more boundary evidence from the raw LiDAR data.

## Changes

### Class 1 (unassigned) inclusion
- Added `isBuildingOrUnassignedClass()` to `_common.js` (class 1, 6, 67)
- Detect-boundary now extracts class 1 points alongside building points
- **Critical fix**: density is still computed from class 6/67 only — class 1
  points are scattered far from buildings and diluted density from 12.8 to 4.7
  pts/m², breaking gap detection entirely (0 hits)

### Phase 0c merged into Phase 0a
- Ground-proximity eave detection (bit 5) was a separate pass with its own k-NN
- Merged into Phase 0a's existing k-NN loop: `testPoint()` now returns
  `{ gap, groundNbCount }` — no extra spatial query needed
- Bit 5 is mutually exclusive with bit 1 (strict eave takes priority)

### Bit 5 — Ground-proximity eave (32)
- Building point with >= 2 ground (class 2) neighbors in k-NN
- Simpler, more inclusive criterion than gap-based eave (bit 1)
- Used by snap-footprint for per-edge snap decisions
- Color: cyan `#00ffcc`

### Bit 6 — Wall-return (64)
- Not-first-return points (returnNumber > 1) near first-return multi-return
  points in XZ projection (grid cell = 2/sqrt(density), 3x3 neighborhood check)
- Physics: when a pulse splits at a building edge, the first return hits the
  roof and subsequent returns continue to the wall/ground below
- Their XZ position marks precisely where the building edge is
- Weight 2.0, color: pink `#ff6699`

### Bit 7 — Multi-return noise (128)
- Not-last-return points that failed Phase 0b clustering (too sparse/isolated)
- Inverse of bit 4: all Phase 0b candidate points that didn't survive
  grid-clustering + connected-components filtering
- Color: bright red `#ff0000` (changed from dark gray `#555555` for visibility)

### Documentation
- Created `detect-boundary.md` alongside the implementation with full pseudo
  code, bit table, exclusion rules, UI rendering priority, consumer table,
  and parameter reference

## Results (dahouet test case)

```
density=12.8 pts/m²
gap=860 (eave=651, dormer=63, obstacle=146)
multi-return=679, mr-noise=158, wall-return=247
ground-prox=705
total=2113 candidates
```

## Bit table (final)

| Bit | Value | Label | Phase |
|-----|-------|-------|-------|
| 0 | 1 | Gap | 0a |
| 1 | 2 | Eave | 0a |
| 2 | 4 | Dormer | 0a |
| 3 | 8 | Obstacle | 0a |
| 4 | 16 | Multi-return | 0b |
| 5 | 32 | Ground-prox | 0a (merged) |
| 6 | 64 | Wall-return | 0b ext |
| 7 | 128 | MR-noise | 0b |
