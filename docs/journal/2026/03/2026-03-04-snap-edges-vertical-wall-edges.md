---
id: "2026-03-04-snap-edges-vertical-wall-edges"
date: 2026-03-04
title: "Snap-Edges Step + Vertical Wall Edges"
tags: [edges, snap-edges, wall-edges, lod2, ground-truth]
category: development
status: concluded
refs: ["2026-03-03-multi-source-edge-detection", "2026-03-03-ridge-clip-unified-step", "2026-03-03-ridge-edge-fit-segment-pairs"]
pipeline: v6a-base
---

## Summary

Two new pipeline steps added:

1. **snap-edges** (v4): Fuses, deduplicates, and snaps multi-source edges into a connected edge graph. Three phases: dedup/fuse, endpoint snapping, gap closing.

2. **compute-wall-edges** (v1): Generates vertical wall edges at footprint corners by matching edge endpoints to footprint vertices. Uses a fixed 3m wall height (placeholder for future BDTOPO data).

Also added vertical wall edges to synthetic ground truth via `addVerticalWallEdges()` in `test-case-builder.js`.

## Snap-Edges Design

Three edge sources have complementary strengths:
- **ridge-clip**: Best position & orientation (plane equations), unreliable extent
- **wall-roof**: Unique perimeter detection capability
- **ridge-fit**: Approximate position, correct extent (actual points)

**Fusion rule**: When ridge-clip and ridge-fit overlap, take ridge-clip's line (position + direction) and project ridge-fit endpoints onto it (extent).

**Phases**:
1. Dedup: angular + perpendicular distance + overlap → union-find grouping. Same-source near-duplicates → keep highest priority. Cross-source RC+RF → fuse.
2. Snap: Cluster nearby endpoints into junctions, replace with centroids.
3. Close gaps: T-junctions (ray→segment) and L-junctions (ray→ray).

All thresholds derived from sensor density.

## Vertical Wall Edges (Ground Truth)

`addVerticalWallEdges(geometry)` in `test-case-builder.js`:
- Indexes all plane vertex positions by XZ coordinate
- For each footprint corner, finds the lowest roof vertex directly above
- Adds `{ type: 'wall', from: ground_level, to: eave_height }` edge

Tested on all 29 roof generators: every footprint corner gets a wall edge.

## Vertical Wall Edges (Pipeline)

`compute-wall-edges` step:
- Reads `edges` (snapped) + `footprint` + `lidar-density`
- For each footprint corner, finds nearest snapped edge endpoint within matchRadius
- Draws vertical edge from `(endpoint.y - 3m)` to endpoint
- Writes `wall-edges` artifact

Inserted after `snap-edges` in all 21 pipelines.

## Files Changed

| File | Change |
|------|--------|
| `lib/pipeline/steps/snap-edges.js` | Created (v4) |
| `lib/pipeline/steps/compute-wall-edges.js` | Created (v1) |
| `lib/pipeline/steps/index.js` | Registered both steps |
| `lib/pipeline/pipelines.js` | Added both steps to all 21 pipelines, `wall-edges` in outputs |
| `lib/pipeline/wiring.js` | Added `compute-wall-edges` wiring in `hullChain()` |
| `lib/synthetic/test-case-builder.js` | Added `addVerticalWallEdges()` helper |

## Results

- synth10 (7 cases): all pass, avg 95.4, no regressions
- `compute-wall-edges` runs in <1ms per case
- No score impact (wall edges are a separate artifact, not consumed by metrics)

## Future Work

- ~~Edge metric (`lib/metrics/edges.js`) comparing GT edges vs detected edges (PCont/RCont/POri/ROri)~~ → Done: see `2026-03-04-edge-accuracy-metric`
- Replace fixed 3m wall height with BDTOPO `hauteur` field
- Regenerate all 240 synthetic test cases to include wall edges in GT
