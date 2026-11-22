---
id: "2026-03-03-immutable-artifact-refactor"
date: 2026-03-03
title: "Immutable Artifact Pattern for Pipeline Steps"
tags: [pipeline, refactor, immutability, debugging]
category: development
status: concluded
refs: ["2026-03-02-unfold-ransac-ridge-bitmask"]
---

## Problem

Six post-hull steps silently mutated the shared `planes` artifact in-place via
object references, without calling `addArtifact`. Similarly, `competitive-grow`
and `point-stealing` mutated the shared `owner` array, and `refit-planes`
mutated `plane-equations` entries.

This caused two problems:
1. **Debugging impossible**: no way to see intermediate states (e.g., which step
   corrupted the church tower planes)
2. **Cache correctness**: steps that mutate without `addArtifact` don't register
   their output — cache replay skips the mutation

## Solution

**Principle**: Every step produces its own output via `addArtifact`. No step
mutates data from a previous step's artifact. Exception: `ridge-candidates`
remains accumulative (OR-bitmask by design).

### Pattern: clone → mutate → register

```js
const planes = clonePlanes(ctx.getArtifact('planes').data);
// ... mutate planes ...
ctx.addArtifact('planes', 'metadata', planes);
```

`clonePlanes()` deep-clones each plane object (normal, hull vertices).

### Changes

**New utility**: `clonePlanes()` in `lib/algorithms/_common.js`

**Post-hull steps** (6 steps, all v2→v3):
- `parallel-edge-clip`: clone before clipping hulls at ridge lines
- `close-corner-gaps`: clone before extending hull vertices
- `clip-to-footprint`: clone before clipping to footprint
- `obstacle-filter`: clone before dissolving micro-planes
- `resolve-overlaps`: clone before resolving overlap geometry
- `shape-fitting`: clone before ridge-constrained shape fitting

**Owner immutability**:
- `competitive-grow` v5→v6: creates fresh `Int8Array` instead of `fill(-1)` on
  shared reference. Registers via `addArtifact('owner', ...)`
- `point-stealing` v5→v6: clones owner via `new Int8Array(...)` before passing to
  `stealPoints()`. Registers via `addArtifact('owner', ...)`

**Plane-equations immutability**:
- `refit-planes` v2→v3: clones `plane-equations` array (spread + deep-clone
  normals) before mutating. Registers via `addArtifact('plane-equations', ...)`

**UI changes**:
- Removed edges from General Controls (users click the `edges` artifact instead)
- Clicking `edges` artifact now toggles 3D edge lines visibility
- `owner` artifact matching uses prefix (`data.name.startsWith('owner')`) for
  future snapshot compatibility

### What remains accumulative (by design)

- `ridge-candidates`: Uint8Array bitmask, OR'd by cascade-ridge-detect,
  competitive-grow, point-stealing
- `seed-planes`: accumulated across seeding steps (cascade, ransac, histogram)
- `plane-equations`: accumulated across seeding steps (only refit clones+mutates)

## Verification

- 1535/1535 tests pass
- Pipeline cache cleared (required after version bumps)
