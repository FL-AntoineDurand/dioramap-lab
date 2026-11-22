---
id: "2026-02-22-ridge-first-detection"
date: 2026-02-22
title: "Ridge-First Detection: Paradigm Dead End"
tags: [ridge-detect, hessian, canny, paradigm]
category: experiment
status: concluded
---

# Ridge-First Detection: Paradigm Dead End

- `ridge-detect.js` — 7-phase pipeline: rasterize, Hessian eigendecomposition, Canny NMS, hysteresis, flood-fill, plane fit, merge
- Development: all zeros (threshold too high) → lowered thresholds → hull-based merge → **86.0 average**
- Excels on simple roofs (flat 99.6, gable 99.2) but fails on hip (44.8), mansard (60.3), T-shaped (62.2)
- **Decision:** RANSAC-grow family confirmed as superior paradigm; ridge-detect archived
- Ridges don't form complete barriers between hip roof planes. Ridge endpoints are ambiguous.
- Code kept for reference but not developed further.
