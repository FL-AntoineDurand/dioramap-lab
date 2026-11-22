---
id: "2026-02-22-v1-champion-corner-gaps"
date: 2026-02-22
title: "V1 Champion Declared: Corner Gap Closing"
tags: [v1, champion, corner-gaps, isOnCorrectSide]
category: development
status: concluded
milestone: "V1 Champion"
metrics:
  score: 98.9
  test_suite: "80 synth"
---

# V1 Champion Declared: Corner Gap Closing

- Created `lib/postprocess/plane-intersection.js` (517 lines): `closeCornerGaps()` + `dilateAndClipHulls()`
- Development: initial regression (95.8) → distance check fix (97.9) → `isOnCorrectSide()` prefilter (**98.9, zero regressions**)
- **V1 Champion: `ransac_grow_clip_reseed_corners` at 98.9 on 80 cases**
- Corners beat Dilate (98.9 vs 98.7) with zero trade-offs on L/T-shaped
- **Key insight:** `isOnCorrectSide()` prefilter against all ridge lines prevents reconvexed hulls from crossing ridges
