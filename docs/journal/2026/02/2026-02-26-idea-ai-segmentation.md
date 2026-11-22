---
id: "2026-02-26-idea-ai-segmentation"
date: 2026-02-26
title: "Idea: AI Segmentation (YOLOv8-obb, MobileSAM)"
tags: [idea, ai, yolov8, mobilesam, onnx, research]
category: idea
status: proposed
---

# Idea: AI Segmentation (YOLOv8-obb, MobileSAM)

## What
Use ONNX-exported ML models to detect roof features from orthophotos.

## Two Approaches
- **YOLOv8-obb**: oriented bounding boxes around ridges, edges, chimneys, solar panels. Precision 0.99, recall 0.96 on roof edges (Mei et al., March 2025, arxiv:2503.09187).
- **MobileSAM**: point-prompted segmentation. Per-plane centroid as prompt → pixel mask. Compare mask vs LiDAR hull for boundary validation.

## Dependencies
`onnxruntime-node` (npm), model files (87 MB for YOLOv8l-obb, 40 MB for MobileSAM).

## Timeline
1-2 weeks for Phase 1 (YOLOv8-obb inference), 2-3 weeks for Phase 2 (MobileSAM integration).

## Risk
Model trained on non-French data may underperform on French building styles (slate roofs, zinc roofs). Fine-tuning on IGN 20cm orthophotos would help but requires labeled training data.

## Relevant Datasets
INRIA Aerial (810 km², 0.3m), ISPRS Potsdam (5cm), Building3D (160K buildings, 60+ roof types), BDAPPV (~50K French PV panel images at 20cm).

## References
arxiv:2503.09187 (YOLOv8 OBB), arxiv:2508.19003 (RoofSeg edge-aware transformer), arxiv:2503.10913 (PolyRoof GNN), ISPRS 2024 (SAMPolyBuild).
