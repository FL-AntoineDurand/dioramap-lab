---
id: "2026-02-25-v3-development-overnight"
date: 2026-02-25
title: "V3 Development: 5 Tracks, 9 Variants (Overnight Session)"
tags: [v3, flat-histogram, categorization, adaptive-ransac, parapet]
category: development
status: concluded
milestone: "V3 Combined"
metrics:
  score: 91.9
  test_suite: "240 synth"
---

# V3 Development: 5 Tracks, 9 Variants (Overnight Session)

## Track Results

- **Track 1 (Flat Histogram):** 4 sub-variants. `histogram.js` shared utils. 1a (Peaks) and 1b (KDE) tied at 88.5; 1c (Spikes) regressed to 87.1. 1a selected (simplest).
- **Track 2 (Normals Reuse):** `normal-analysis.js`. Marginal +0.2, +5% runtime. Excluded from V3.
- **Track 3 (Early Categorization):** `plane-categorizer.js` classifying roof/parapet/chimney/ac_unit/vent/antenna. Asymmetric stealing rules. **Best individual track: +0.6**.
- **Track 4 (Adaptive RANSAC):** Per-iteration parameter schedules, maxReseedIter 3->4. Solid +0.3.
- **Track 5 (Parapet-Aware):** Fully subsumed by Track 3.

## V3 Combined

`ransac-grow-clip-reseed-v3.js`: Track 1a + Track 3 + Track 4. 7-phase pipeline.

**Critical bug fix discovered during V3 benchmarking:** `resolveOverlappingPlanes()` sorted planes by hull area instead of point count. A sparse parapet (915 pts, 1487 m2 hull) was ranked above the main roof (43K pts, 1460 m2 hull), causing the roof to be eliminated. Fix: sort by point count (area as tiebreaker). Impact: ALL algorithms +1.0 pt; V3 +1.4 pt.
