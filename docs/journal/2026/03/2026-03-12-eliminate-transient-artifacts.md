---
id: "2026-03-12-eliminate-transient-artifacts"
date: 2026-03-12
title: "Eliminate transient artifacts: wire kd-tree, localize RNG"
tags: [pipeline, wiring, refactor, kd-tree, rng, transient]
category: development
status: concluded
refs:
  - "2026-03-04-qualified-names-explicit-wiring"
  - "2026-03-03-immutable-artifact-refactor"
  - "2026-03-12-tile-signals-unified-pca"
---

## Problem

Three `_`-prefixed transient artifacts bypassed the explicit wiring system:

| Artifact | Producer(s) | Consumers | Risk |
|----------|-------------|-----------|------|
| `_kdTree` | compute-normals, compute-tile-normals, competitive-grow | 12 steps (cascade-*, grow, steal, ridge-edge-fit, strip-region-grow) | **Singleton collision**: tile chain's kd-tree silently overwritten by building chain's in exploration pipeline |
| `_rng` | ransac-seed, histogram-seed, cascade-ransac-seed | Same 3 steps (lazy-init pattern) | Shared mutable state across repeat blocks |
| `_tileKdTree` | compute-tile-normals | None | Dead code, never consumed |

Transient artifacts stored as bare names (no `stepLabel.` qualification), making them invisible to wiring validation, cache introspection, and artifact listing. The last-writer-wins semantics meant execution order silently determined which kd-tree consumers received.

## Solution

### `_rng` — removed entirely

Each seed step now creates a local `createRng(params.seed ?? 42)` instead of sharing a singleton. This is correct because:
- Each step instance processes a different point subset
- RNG state doesn't need to persist across steps
- Determinism is preserved via the seed parameter

### `_tileKdTree` — deleted

Dead artifact. compute-tile-normals produced it, but no step ever read it. Removed without replacement.

### `_kdTree` — converted to wired `kd-tree`

The kd-tree is now a proper wired artifact with qualified names (`compute-normals#0.kd-tree`, `tile-signals#0.kd-tree`).

**Producers** (write-only):
- `compute-normals` — builds from building-points
- `tile-signals` — builds from all tile positions
- `compute-tile-normals` — builds from all tile positions (legacy)

**Lazy-init consumers** (read + fallback write):
- `competitive-grow`, `point-stealing`, `asymmetric-steal` — read from wired source if available, build locally if not (for v2/v3 pipelines without compute-normals)

**Pure consumers** (read-only):
- 3 cascade segment variants, `cascade-ridge-detect`, `strip-region-grow`, `ridge-edge-fit`

**Wiring strategy — fan-out from producer, not chain:**

All consumers in a chain wire directly to the producer (`CN` or `CTN`), not to the previous step in execution order. This is safe because:
1. `building-points` is immutable — no step modifies it after extract-denoise
2. The kd-tree built from building-points is therefore identical regardless of when it's built
3. No consumer modifies the kd-tree — they only query it

Chain wiring (each step reading from the previous) would only be needed if intermediate steps rebuilt the kd-tree on a filtered/modified point set, which doesn't happen.

**Exploration pipeline — two distinct kd-trees:**

The exploration pipeline has separate tile and building chains. With transient singletons, the building chain's kd-tree silently overwrote the tile chain's (by execution order). With proper wiring, each chain's consumers are explicitly wired to their producer:
- Tile chain consumers → `tile-signals#0`
- Building chain consumers → `compute-normals#0`

### `rebuildTransients` — wiring-aware rebuild

The cache rebuild method now iterates the wiring table to find kd-tree source steps and rebuilds from their `building-points`. Uses a `rebuiltSources` Set to avoid duplicate rebuilds when multiple consumers wire to the same source.

## Bug discovered: silent empty normals

During migration, `compute-normals` was writing kd-tree via `addArtifact('kd-tree', ...)` then reading it back via `getArtifact('kd-tree')`. After the rename from `_kdTree`, `getArtifact` resolves through wiring — but `compute-normals#0` has no kd-tree wiring source (it's a producer, not a consumer). The read returned `null`, causing the step to exit early with empty normals.

**Impact**: All V4+ pipelines were silently running with empty normals, falling back to pure RANSAC seeding. The cascade segmentation was never engaging. Scores were lower than they should have been (flat: 1p→2p, multi_section: 4p→6p after fix).

**Fix**: Build kd-tree into a local variable, use it directly in the PCA loop, and only call `addArtifact` to publish for downstream consumers. Removed `kd-tree` from the `reads` list since compute-normals only produces it.

## Verification

All 22 pipelines pass on the quick test set with zero failures. Tested: v2-champion (no compute-normals, CG lazy-init), v3-combined (CG lazy-init with kd-tree from CG), v6a-base, v6d-progressive, v6x-hybrid, v6g-kde-silverman, exploration (dual kd-tree chains).

## Files changed

- `lib/pipeline/context.js` — rebuildTransients rewritten
- 3 seed steps — `_rng` removed
- `compute-normals` — producer pattern, bug fix
- 3 lazy-init steps — read-or-build pattern
- 6 pure consumer steps — `_kdTree` → `kd-tree`
- `lib/pipeline/wiring.js` — kd-tree entries for all 22 pipelines
- `extract-denoise` — comment update
