---
id: "2026-03-06-axis4-curvature-pca-region-growing"
date: 2026-03-06
title: "Axis 4: PCA Curvature Classification & Region Growing"
tags: [research, plane-detection, pca, curvature, region-growing, eigenvalues]
category: experiment
status: concluded
refs: ["2026-03-06-four-axis-research-synthesis"]
---

## Summary

Explored plane detection via PCA eigenvalue ratios (planarity/linearity/sphericity) for point classification, planarity-sorted region growing with periodic PCA re-fit, and a 16-stage post-processing pipeline. The most intensively explored axis — 42 versions over ~7 hours.

## Champion: v0.42.0 — 98.2 quick, 96.4 all-synth

### Architecture (16-stage pipeline)
1. PCA curvature classification (k=20 neighbors)
2. Planarity-based seed selection (planarity > 0.5, sorted descending)
3. Region growing with periodic PCA re-fit (every 50 points)
4. Adaptive distance threshold (max(0.15m, 3*localNoise))
5. Connected-component post-split (k=20)
6. Size-aware two-phase merge (standard + fragment absorption)
7. Steal-back boundary refinement
8. Offset histogram split (Otsu bimodal)
9. Post-absorption height outlier split (MAD-based)
10. Silo protection (curvature-variation-aware absorption tightening)
11. Aggressive residual recovery (relaxed angle, no-normal assignment)
12. Mini-plane recovery (4-point minimum clusters)
13. Anti-normal ray convergence (cylinder/sphere detection)
14. Small surface spatial re-split (k=5 CC, RMS>0.05 guard, v33)
15. Collinear normal inheritance (l2/l1<0.05, v28)
16. Selective micro-fragment absorption (RMS<0.05, parapet-protected, v42)

### Key Breakthroughs (chronological)
- **k=20 neighbors** (v0.1→v0.3): +2 composite over k=15
- **Steal-back boundary refinement** (v0.4): +0.2 avg, gambrel +1.7
- **Connected-component post-split** (v0.5): spatial separation of coplanar regions
- **Two-phase merge** (v0.11): standard + fragment absorption, no regressions
- **Offset histogram with Otsu** (v0.15): flat_with_antennas-small 78.6→96.9 (+18.3)
- **Collinear normal inheritance** (v0.28): flat_with_high_parapet-small 80.6→95.7 (+15.1)
- **Spatial re-split with RMS guard** (v0.33): flat_with_antennas-large 84.7→96.9 (+12.2), zero regressions
- **Selective absorption** (v0.42): flat_with_high_parapet-default +0.9, zero regressions

### What Didn't Work (20+ abandoned experiments)
- Normal histogram seeding (v0.8): helps silo, hurts complex roofs
- Tighter growing angle=8 (v0.9, v0.43): creates unrecoverable fragments for hip/mansard
- Propagated normal orientation (v0.10): cascading flip errors, -1.9 avg
- Centroid-to-plane merge (v0.20): breaks parapets (-8.9)
- Micro-fragment absorption without parapet guard (v0.36): net negative (-0.1 avg)
- Relaxed final merge (v0.40): row_houses -18.8, silo -20.0, split_level -22.7
- Trial-fit merge: coplanar planes have identical combined RMS, merge incorrectly

### Structural Limitations (documented, unfixable at detector level)
- **Parapet contamination**: PCA through mixed parapet+slope points gives 40-66 deg tilted normal with low RMS — can't detect bad fit
- **Tiny obstacle floor**: vents (1-3pts), antennas (5-8pts) below any practical threshold
- **Chimney contamination**: 19pts with 6 contaminants → 23.5 deg off (just past 15 deg scorer threshold)

### Score Progression
| Version | All-Synth | Key Change |
|---------|-----------|------------|
| v0.1 | ~95.7 | Initial PCA + region growing |
| v0.5 | 95.8 | CC split + adaptive distance |
| v0.13 | 95.8 | Height split + MAD |
| v0.18 | 96.1 | Offset histogram + silo protection |
| v0.28 | 96.3 | Collinear normal inheritance |
| v0.33 | 96.35 | Spatial re-split with RMS guard |
| v0.42 | **96.39** | Selective micro-fragment absorption |

## Files
- Detector: `research/axis-4-curvature/detector.js` (v0.42)
- Results: `research/axis-4-curvature/results/all-synth-v33.json` (96.4)
- Full findings: `research/axis-4-curvature/FINDINGS.md`
