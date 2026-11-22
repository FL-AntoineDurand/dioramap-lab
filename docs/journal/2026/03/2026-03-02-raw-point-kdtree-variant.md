---
id: "2026-03-02-raw-point-kdtree-variant"
date: 2026-03-02
title: "Build kdTree on raw LiDAR points instead of denoised building points"
tags: [architecture, kdtree, denoising, experiment]
category: idea
status: proposed
refs: ["2026-03-02-extract-denoise-lazy-init-decoupling"]
---

## Motivation

Currently the `_kdTree` is built on denoised building points (class-6, footprint-filtered, grid-denoised). This means:

1. **Nearest-neighbor queries only see denoised points** — if a point was removed by denoising, its neighbors won't find it
2. **Wall/ground points are invisible** to the growing algorithm — competitive-grow can't detect transitions from roof to wall
3. **Denoised points may have different neighborhood structure** than the original cloud

Building the kdTree on ALL raw LiDAR points (or at least all class-6 points before denoising) could give:

- Better boundary detection at roof edges (wall points as natural boundaries)
- More accurate neighborhood for normals computation near edges
- Potential improvement on complex roofs where denoising removes valid points

## Approach

The lazy-init decoupling (see ref) makes this straightforward:

1. **Create a new variant** (e.g. `v6a-raw-kd`) that builds `_kdTree` on the full `positions` cloud instead of `building-points`
2. **Use `denoised-to-orig` cross-reference** to map between building-point indices and original indices when needed
3. **Benchmark** on the 643 test cases: compare scores to `v6a-base`

### Implementation options

**Option A**: Build kdTree on all positions (class 2+6+...). Pros: wall boundaries visible. Cons: ground clutter adds noise, larger tree.

**Option B**: Build kdTree on all class-6 points (pre-denoise). Pros: no ground clutter, captures removed-but-valid points. Cons: noise points still in tree.

**Option C**: Parameterize — add a `kdTreeSource` param (`'denoised'` | `'class6'` | `'all'`) and test all three.

## Risk

Unknown whether this helps or hurts. The current denoised-only approach works well (98.9 champion score). Raw points include noise that was intentionally removed. Need empirical evidence.

## Dependencies

- Lazy-init decoupling must be complete (done)
- The step that builds `_kdTree` needs access to `positions` artifact (currently only reads `building-points`)
