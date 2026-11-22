---
id: "2026-03-02-extract-denoise-lazy-init-decoupling"
date: 2026-03-02
title: "Decouple extract-denoise from downstream artifact initialization"
tags: [architecture, refactoring, lazy-init, pipeline]
category: development
status: concluded
refs: []
---

## Context

`extract-denoise` was responsible for creating 9 artifacts: its own 3 core outputs (`building-points`, `building-count`, `denoised-to-orig`) plus 6 downstream shared-state artifacts (`owner`, `seed-planes`, `plane-equations`, `_kdTree`, `_rng`, `min-inliers`). This coupling meant:

1. Every pipeline *had* to use extract-denoise's initialization, even if the downstream step would compute different values
2. Params like `seed` and `minInliersPct` leaked into extract-denoise despite belonging to seeding steps
3. Pipelines without denoising still needed extract-denoise for the shared state init
4. The `_kdTree` was always built on denoised building points, with no option to build on raw points

## Change

Adopted **Proposal C: lazy-init in consuming steps**. Each consuming step now guards artifact creation with:

```js
if (!ctx.hasArtifact('owner')) {
  ctx.addArtifact('owner', 'array', new Int8Array(totalPts).fill(-1), { sourceStep: stepName });
}
```

### Artifact migration

| Artifact | Moved to (lazy-init) |
|----------|---------------------|
| `_kdTree` | compute-normals, competitive-grow, point-stealing, asymmetric-steal |
| `owner` | normal-cascade-segment, ransac-seed |
| `seed-planes` | ransac-seed, histogram-seed, cascade-ransac-seed, normal-cascade-seed, normal-histogram-seed |
| `plane-equations` | same 5 steps as seed-planes |
| `_rng` | ransac-seed, histogram-seed, cascade-ransac-seed |
| `min-inliers` | ransac-seed, histogram-seed |

### Files modified (10 steps)

- `extract-denoise.js` — removed 6 artifact creations, removed `seed`/`minInliersPct` params
- `compute-normals.js` — added `_kdTree` lazy-init
- `normal-cascade-segment.js` — added `owner` lazy-init
- `competitive-grow.js` — added `_kdTree` lazy-init + `buildKdTree` import
- `point-stealing.js` — added `_kdTree` lazy-init + `buildKdTree` import
- `asymmetric-steal.js` — added `_kdTree` lazy-init + `buildKdTree` import
- `ransac-seed.js` — added 5 lazy-inits + `seed` param in meta
- `histogram-seed.js` — added 5 lazy-inits + `seed` param in meta
- `cascade-ransac-seed.js` — added 3 lazy-inits + `seed` param in meta
- `normal-histogram-seed.js` — added 2 lazy-inits

### Tests

- Updated `steps.test.js`: removed stale artifact checks from extract-denoise test
- Added 3 new lazy-init tests verifying: compute-normals creates `_kdTree`, ransac-seed creates all 5 shared artifacts, competitive-grow creates `_kdTree` in v2-champion path
- Result: 1456 pass, 4 fail (all pre-existing)

### Docs updated

- `STEP-CATALOG.md` — extract-denoise writes list reduced to 3, params cleaned
- `ARTIFACT-TYPES.md` — producer columns updated for all 6 migrated artifacts
- `ARCHITECTURE.md` — rebuildTransients description updated

## Next step

The `_kdTree` is still built on denoised building points (same behavior as before). A future variant will build it on all raw LiDAR points and benchmark the difference via `denoised-to-orig` cross-reference.
