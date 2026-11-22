---
id: "2026-03-04-edge-accuracy-metric"
date: 2026-03-04
title: "Edge Accuracy Metric (PCont/RCont/POri/ROri)"
tags: [edges, metrics, lod2, pyscoring]
category: development
status: concluded
refs: ["2026-03-04-snap-edges-vertical-wall-edges", "2026-03-03-multi-source-edge-detection"]
pipeline: v6a-base
---

## Summary

New standalone edge accuracy metric module `lib/metrics/edges.js` implementing PyScoring conventions (PCont, RCont, POri, ROri) from the LOD2 CityGML benchmark, adapted for 3D edge segments.

Not plugged into `computeAllMetrics()` yet — call standalone via `computeEdgeMetrics(gtEdges, detEdges, opts)`.

## Algorithm

1. **Sample** both GT and detected edge sets at regular intervals (`sampleStep`, default 0.1m)
2. **RCont** (recall contour): For each GT sample, find nearest detected sample → mean distance
3. **PCont** (precision contour): For each detected sample, find nearest GT sample → mean distance
4. **Completeness**: Fraction of GT samples within `distThreshold` (default 0.5m) of any detected sample
5. **Precision**: Fraction of detected samples within `distThreshold` of any GT sample
6. **POri/ROri** (orientation): Match each edge to its nearest counterpart by midpoint distance, compute unsigned angular error (0–90°, edges are undirected)
7. **Per-edge detail**: Arrays with `nearestGtIdx`/`nearestDetIdx`, `angleDeg`, `midpointDist` for diagnostics

## Return Value

```javascript
{
  pCont, rCont,           // mean directed distances (m)
  pOri, rOri,             // mean angular errors (degrees)
  completeness, precision, // fractions [0,1]
  gtEdgeCount, detEdgeCount,
  gtTotalLength, detTotalLength,
  perDetEdge, perGtEdge,  // per-edge detail arrays
}
```

## Edge Cases

- Both empty → completeness=1, precision=1 (vacuous truth)
- GT empty, det non-empty → completeness=1, precision=0, pCont=Infinity
- Det empty, GT non-empty → completeness=0, precision=1, rCont=Infinity
- Zero-length edges → handled gracefully (single sample point)
- `excludeTypes` option filters GT edges by type (e.g. `['wall']`)

## Unit Tests

28 tests in `test/metrics-edges.test.js` with procedurally created synthetic edges:

| Group | Tests | What it verifies |
|-------|-------|-----------------|
| VERSION | 1 | Exports positive integer |
| Empty inputs | 3 | Both empty, GT-only, det-only |
| Perfect match | 3 | Identical, multi-edge, reversed direction |
| Known offset | 3 | 0.3m offset, 1.0m beyond threshold, custom threshold |
| Known angle | 3 | 45°, 90°, 30° 3D tilt |
| Partial match | 3 | Half coverage, extra detections, missing GT |
| Per-edge detail | 2 | Nearest index correctness |
| excludeTypes | 2 | Single type, multiple types |
| Vertical wall edges | 3 | Perfect, shorter, horizontal vs vertical |
| sampleStep | 2 | Coarse and fine sampling |
| Degenerate edges | 2 | Zero-length GT and det |
| Gable wireframe | 1 | 7-edge realistic scenario with small offsets |

All 28 pass.

## Files

| File | Change |
|------|--------|
| `lib/metrics/edges.js` | Created — `VERSION = 1`, `computeEdgeMetrics()` |
| `test/metrics-edges.test.js` | Created — 28 unit tests |

## Future Work

- Plug into `computeAllMetrics()` when pipeline exposes detected edges in results
- Add to `getMetricVersions()` in `lib/metrics/index.js`
- Consider KD-tree for nearest-neighbor queries if performance matters on large edge sets
- Add to composite score (opt-in for LOD2 pipelines)
- Per-type breakdown (ridge/eave/hip/wall completeness separately)
