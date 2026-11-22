---
id: "2026-02-26-idea-parapet-pre-detection"
date: 2026-02-26
title: "Idea: Parapet-Aware Detection (Explicit Pre-Detection)"
tags: [idea, parapet, pre-detection, footprint-edge, retry]
category: idea
status: proposed
---

# Idea: Parapet-Aware Detection (Explicit Pre-Detection)

## Original Attempt
Track 5 (`parapet_aware`) — standalone parapet detection with asymmetric stealing after RANSAC.

## Why It Failed
Subsumed by Track 3's general categorization system.

## Why Retry
Track 3 categorizes AFTER RANSAC converges. By then, parapets have already stolen edge points. An explicit PRE-detection step could identify parapet candidates BEFORE growing starts, using building edge proximity + narrow PCA + elevated above roof.

## How to Retry
New step `pre-detect-parapets.js` running after `denoise` but before `ransac-seed`. During `competitive-grow`, parapet candidates have a reduced steal weight (0.5x).

## V6 Note
V6 Normal Cascade partially addresses this — near-vertical normals (tilt ~80-90°) form their own normal group and get seeded as separate planes BEFORE competitive growing. The remaining benefit of explicit pre-detection would be the reduced steal weight during growing.

## Expected Impact
+2-5 pts on `flat_large_parapet` (79 -> 84+) and `mono_pitch_with_parapet` (87 -> 90+).

## Difficulty
Medium (new step + modify grow logic).
