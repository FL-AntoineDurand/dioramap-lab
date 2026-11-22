---
id: "2026-02-26-idea-subpixel-alignment"
date: 2026-02-26
title: "Idea: Sub-Pixel Alignment Refinement"
tags: [idea, alignment, sub-pixel, cross-correlation]
category: idea
status: proposed
---

# Idea: Sub-Pixel Alignment Refinement

## What
Add parabolic interpolation to the DSM cross-correlation alignment to achieve sub-pixel precision.

## Where Mentioned
ALIGNMENT-RESEARCH, ALGORITHM-GENEALOGY §4.3.

## Why Not Yet Implemented
Trivial to add to cross-correlation. Current pixel-level alignment is sufficient for benchmarking.

## Expected Impact
Low. Current pixel-level alignment is sufficient. Would matter for production use with high-precision requirements.
