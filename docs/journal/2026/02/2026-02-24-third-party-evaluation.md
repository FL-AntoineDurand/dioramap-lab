---
id: "2026-02-24-third-party-evaluation"
date: 2026-02-24
title: "Third-Party Tool Evaluation: Our Champion Beats All"
tags: [open3d, city3d, roofer, polyfit, third-party, comparison]
category: benchmark
status: concluded
---

# Third-Party Tool Evaluation: Our Champion Beats All

- Created `lidar-third-parties/` evaluation harness
- Evaluated: Open3D (Python RANSAC), City3D (C++ CGAL, built from source), Roofer (C++ CGAL 6.0, built from source), PolyFit (unusable — needs pre-segmented .vg)

| Tool | Score (20 cases) |
|------|:----------------:|
| Our champion | 98.9 |
| Roofer | 92.2 |
| Open3D | 90.0 |
| City3D | 56.0 |
| PolyFit | N/A (unusable) |

- Verdict: our champion beats all third-party tools, especially on complex roofs
