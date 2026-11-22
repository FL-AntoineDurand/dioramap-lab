---
id: "2026-03-02-v6g-kde-pipeline-variants"
date: 2026-03-02
title: "V6G-KDE Pipeline Variants Added for Normal Cascade"
tags: [v6, kde, pipeline, variants]
category: development
status: concluded
refs: ["2026-03-02-v6-kde-stage1-wins-stage2-loses"]
pipeline: v6g-kde
design_doc: "docs/V6-KDE.md"
---

# V6G-KDE Pipeline Variants Added for Normal Cascade

Registered V6G-KDE pipeline variants for Stage 1 and Stage 2 KDE experiments:

- 5 Stage-2 KDE variants (offset splitting): Silverman, IQR, physical, adaptive, posterior bandwidth strategies
- 3 Stage-1 KDE variants (normal orientation): separable, 2D, physical methods

After the cascade step split, KDE variants use dedicated step wrappers: `normal-cascade-segment-kde-offset` (5 Stage 2 pipelines) and `normal-cascade-segment-kde` (3 Stage 1 pipelines). All share orchestration in `cascade-shared.js`. See the experiment entry for benchmark results — Stage 2 all regress, Stage 1 all win.
