---
id: "2026-02-26-idea-real-building-ground-truth"
date: 2026-02-26
title: "Idea: Real Building Ground Truth Annotation"
tags: [idea, ground-truth, annotation, consensus, research]
category: idea
status: proposed
---

# Idea: Real Building Ground Truth Annotation

## What
Manually annotate ground truth for 10-20 representative real buildings covering different roof types.

## Why
Only 240 synthetic + 2 early real buildings have verified ground truth. The 40 real buildings can only be evaluated visually or with GT-free V2 metrics. Without GT on real buildings, we cannot compute detection rate, segmentation F1, or boundary IoU.

## Alternative
Use high-confidence consensus as pseudo-ground-truth. If V2, V3, and V5 all agree on a building (same plane count, similar boundaries), treat that consensus as ground truth. Only manually annotate where algorithms disagree.

## Timeline
5-10 hours for 20 buildings.
