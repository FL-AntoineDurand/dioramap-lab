---
id: "2026-03-11-hole-aware-solar-panel-fitting"
date: 2026-03-11
title: "Hole-aware solar panel fitting + 3D panel viewer"
tags: [solar, panel-fitting, plane-intersection, procedural, ui]
category: development
status: concluded
refs: []
---

## Problem

Solar panels were placed on dormer cutout areas where no roof surface exists.

## Changes

### plane-intersection (lib/postprocess/plane-intersection.js)

Height-aware overlap resolution: same-level overlaps → clip; on-top overlaps → hole cut. `resolve-overlaps` step now stores plane holes (`hullHoles`). `_bridgeHoles` converts multi-ring polygons to single-ring representation for downstream consumers.

### fit-solar-panels v2 (lib/pipeline/steps/fit-solar-panels.js)

- `convexHull2D` before inset
- Batch exclusion: subtracts all other plane hulls in a single polygon-clipping pass
- Hole-aware scanline rejection: panels with corners inside holes are rejected

### Procedural buildings

GT generator now cuts dormer pentagons from main roof plane ground truth.

### UI

- `solar-panels` artifact type with 3D quad rendering
- Per-plane toggle checkboxes
- Graph view support for solar panel artifacts

Files: 7 files, +632 −179 lines.
