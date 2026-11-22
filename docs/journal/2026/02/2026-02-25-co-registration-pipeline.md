---
id: "2026-02-25-co-registration-pipeline"
date: 2026-02-25
title: "Co-Registration Pipeline: ICP and WECS Metric"
tags: [co-registration, icp, wecs, alignment]
category: development
status: concluded
---

# Co-Registration Pipeline: ICP and WECS Metric

- `lib/geometry/icp-2d.js` -- 2D rigid ICP for footprint-to-LiDAR alignment
- WECS (Wall-Edge Coincidence Score) as intrinsic alignment quality metric
- `pipeline_v2_coreg`: V2 + ICP. Real buildings: 7 wins, 29 ties, 3 losses (+0.1 avg)
