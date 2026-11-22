---
id: "2026-02-26-idea-color-plane-validation"
date: 2026-02-26
title: "Idea: Color-Based Plane Validation (Ortho RGB)"
tags: [idea, ortho, color, validation, over-segmentation]
category: idea
status: proposed
---

# Idea: Color-Based Plane Validation (Ortho RGB)

## What
Use per-plane RGB statistics from the orthophoto to validate or reject plane boundaries. If two adjacent planes have similar color distributions, they may be over-segmented.

## Where Mentioned
NEXT-STEPS Phase 1, ALGORITHM-GENEALOGY §4.3.

## Why Not Yet Implemented
Ortho fetching works, but color validation step not yet built.

## Expected Impact
Medium. Could catch over-segmentation where LiDAR geometry alone is ambiguous.
