---
id: "2026-02-24-overlap-resolution"
date: 2026-02-24
title: "Overlap Resolution Post-Processing"
tags: [overlap, post-processing, boolean-ops]
category: development
status: concluded
refs: ["2026-02-25-overlap-sort-bug"]
---

# Overlap Resolution Post-Processing

- `resolveOverlappingPlanes()`: sort by area, detect containment via boolean ops, eliminate false planes, clip overlaps
- Building 93006: overlap 35.3% to 0.06%, V2 82.7 to 87.3 (+4.6), zero synthetic regression
- Note: sorted by hull area (not point count) — will cause critical bug discovered Feb 25 (see overlap-sort-bug entry)
