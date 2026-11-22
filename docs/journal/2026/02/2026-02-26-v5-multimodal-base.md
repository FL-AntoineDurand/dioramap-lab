---
id: "2026-02-26-v5-multimodal-base"
date: 2026-02-26
title: "V5 Multimodal Base Pipeline"
tags: [v5, multimodal, dsm, ortho, bdtopo, metadata]
category: development
status: concluded
pipeline: v5-multimodal-base
---

# V5 Multimodal Base Pipeline

- 22-step pipeline combining DSM alignment (auto: cross-corr first, ICP fallback), normal-histogram seeding, V3 categorization, asymmetric stealing, ortho + BDTOPO metadata
- New steps: `compute-ortho-artifacts` (RGB, YCbCr, Sobel, Laplacian, footprint mask), `fetch-metadata` (BDTOPO WFS)
- 47 total parameters
- Designed for real buildings with Lambert93 coordinates
