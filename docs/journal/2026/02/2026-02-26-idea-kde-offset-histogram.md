---
id: "2026-02-26-idea-kde-offset-histogram"
date: 2026-02-26
title: "Idea: KDE for Normal Cascade Offset Histogram (V6-KDE)"
tags: [idea, kde, histogram, v6, bandwidth, retry]
category: idea
status: concluded
refs: ["2026-03-02-v6-kde-stage1-wins-stage2-loses"]
design_doc: "docs/V6-KDE.md"
---

# Idea: KDE for Normal Cascade Offset Histogram (V6-KDE)

## Original Attempt
Track 1b (`flat_kde`) — Gaussian KDE instead of fixed-width histogram for elevation peak detection on flat roofs. Same score as simple binning, dropped.

## Why Retry
The V6 Normal Cascade uses 1D offset histograms (Stage 2) on **small subgroups** (20-200 points per orientation cluster). These are the most vulnerable to sparse-data artifacts. KDE eliminates bin boundaries entirely.

## Design Document
Full design in [V6-KDE.md](../../V6-KDE.md) — covers bandwidth selection strategies (Silverman, IQR, physical prior, adaptive), peak detection thresholds, point assignment methods.

## Outcome
Tested in Mar 2. Stage 2 KDE all regress. Stage 1 KDE clearly wins (95W/45L, 2.1:1 ratio). See referenced experiment entry.

## Difficulty
Medium.
