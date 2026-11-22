---
id: "2026-03-09-boundary-co-registration"
date: 2026-03-09
title: "Boundary-RMS co-registration replaces DSM cross-correlation"
tags: [co-register, boundary, pipeline, exploration]
category: development
status: concluded
refs: []
---

## What changed

### co-register step rewrite (v4)

Replaced DSM cross-correlation + ICP approach with boundary-RMS grid search using detect-boundary candidates. The old `compute-dsm-artifacts` step and `v2-co-register`/`v2-dsm-coreg` pipelines were removed.

New step: `detect-boundary` — extracts boundary candidates for the co-registration grid search.

### New: exploration pipeline

Added an exploration pipeline with step annotations and a pass-2 orphan branch. UI renders annotation bars per step.

### Cascade param tuning

Exposed `cascadePeakMinFrac` and `cascadeMinSegmentPts` as tunable params on all 3 cascade variants (`normal-cascade-segment`, `-kde`, `-kde-offset`), with sensor-derived override support.

### Other changes

- `shape-fitting` v6: fix duplicate vertices after endpoint snapping
- Benchmark timing switched to CPU time (`process.cpuUsage`)
- Pipeline context: write enforcement added
- `scope-to-building` step added (new)
- `wall-roof-detect`: large simplification (326 lines removed), reads boundary-candidates from detect-boundary
- UI: co-register footprint overlay, artifact summary fallback, auto-cache-clear on `lib/` changes
- GLOSSARY.md added

### Files changed

31 files, +1323 −1099 lines. Key: `co-register.js` rewritten, `compute-dsm-artifacts.js` deleted, `detect-boundary.js` added, `scope-to-building.js` added.
