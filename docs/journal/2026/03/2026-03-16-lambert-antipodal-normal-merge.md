---
id: "2026-03-16-lambert-antipodal-normal-merge"
date: 2026-03-16
title: "Lambert hemisphere: unoriented normal handling via antipodal group merge"
tags: [segmentation, lambert, normals, pca, walls, v7]
category: bugfix
status: concluded
refs:
  - "2026-03-12-spherical-kde-scan-line-compensation"
  - "2026-03-12-tile-signals-unified-pca"
pipeline: v7
---

# Lambert hemisphere: unoriented normal handling via antipodal group merge

## Problem

PCA-computed surface normals are **unoriented** — they define a line through
the origin (an axis), not a direction. For the same planar surface, PCA can
return either (nx, ny, nz) or (-nx, -ny, -nz) depending on the local point
neighborhood. This is a fundamental property of eigenvector decomposition.

The Lambert hemisphere projection in `hemisphere-normals-density-aware-segment`
(Stage 1) canonicalizes normals by flipping to the upper hemisphere
(`if ny < 0, flip all components`). For roof surfaces (tilt < 70°), this works
because ny is large and its sign is stable. For **wall surfaces** (tilt ≈ 90°),
ny ≈ 0 and its sign jitters due to noise — some points flip to (nx, +ε, nz),
others stay at (-nx, -ε, -nz) and flip to (nx, +ε, nz) via abs(ny). But the
XZ components are NOT flipped consistently, so the two groups end up on
**opposite sides of the Lambert disc**.

### Visual symptom

In the UI, a single wall surface appears as two separate cascade segments with
opposite-sense normals, producing two separate planes in `normal-cascade-seed`.
On Dahouet (real-22186-dahouet-502), the original groups 1 and 3 had:
- Group 1: normal (-0.626, 0.118, 0.771), tilt=88.8°, az=317.6°
- Group 3: normal (0.663, 0.093, -0.743), tilt=88.8°, az=137.6°
- dot(n1, n3) = -0.977 — near-exact antipodal

### Terminology

A vector has a **direction** (the line it lies on) and a **sense** (which way
along that line). Two vectors with the same direction but opposite sense are
called antipodal. An unoriented normal — a direction without a preferred
sense — is a point on the **projective plane** (the sphere with antipodal
points identified).

## Approaches tried

### 1. Canonicalize by ny sign (original)

`lambertProject`: `if (ny < 0) flip all 3 components`.

**Fails for walls**: ny ≈ 0 jitters across the threshold. Points on the same
wall get different canonical orientations depending on which side of ny=0
their PCA normal falls.

### 2. Canonicalize by largest-magnitude component

Flip so the largest |component| is positive:
```
if |ny| ≥ |nx| and |ny| ≥ |nz|: flip by ny sign
else if |nx| ≥ |nz|: flip by nx sign
else: flip by nz sign
```

**Fixed walls, broke 45° roofs**: at tilt ≈ 45°, |ny| ≈ |nx| ≈ 0.7. The
dominant component switches between ny and nx due to noise, causing the same
roof face to split at the canonicalization boundary.

### 3. Dual-splat accumulation

Accumulate both the normal AND its antipodal (half weight each) into the
Lambert grid. The grid becomes symmetric — both orientations contribute to
the same density peak.

**Correct accumulation, but peaks duplicated**: every real peak appears twice
on the disc (one per hemisphere). Required deduplication.

### 4. Dual-splat + dot-product deduplication

After finding peaks, merge pairs with `dot(normal_i, normal_j) < -0.9`.

**Failed**: `lambertInverse` always returns normals in the upper hemisphere
(ny ≥ 0). Two peaks from opposite hemispheres both get ny > 0 normals —
their dot product never reaches -0.9 because both Y components are positive.

### 5. Dual-splat + disc-position deduplication

Merge peaks at near-antipodal disc positions: `|u_i + u_j| < threshold` and
`|v_i + v_j| < threshold`.

**Made things worse** (58 → 71 groups, 24 → 75 antipodal pairs): non-exact
normal antipodality causes peaks at slightly offset disc positions. No
threshold works for all cases — too tight misses real pairs, too loose merges
distinct surfaces.

### 6. abs(ny) canonicalization + antipodal+coplanar post-merge

Keep simple `abs(ny)` canonicalization. After Stage 1 grouping, scan for
group pairs with antipodal peak normals (dot < -0.85) AND coplanar centroids
(centroid offset diff < 3× distThreshold).

**Fixed walls partially** (10 groups merged), but coplanar check rejected
legitimate wall pairs. Root cause: opposite-sense PCA normals don't distribute
uniformly across a surface. One sense clusters on the upper-left of the wall,
the other on the lower-right. Their centroids differ by ~1m even on the same
wall plane, exceeding the 0.27m threshold.

### 7. abs(ny) canonicalization + antipodal-only post-merge ← FINAL

Drop the coplanar check. Merge groups purely on normal antipodality
(dot < -0.85). Rely on downstream stages to handle the rest:
- **Stage 2 (offset KDE)** separates parallel planes at different offsets
- **Stage 3 (spatial CC)** separates physically distinct walls

## Final implementation

### Lambert projection (lib/geometry/lambert.js)

Reverted to the original simple canonicalization:
```js
const sign = normal.y < 0 ? -1 : 1;
const nx = normal.x * sign, ny = normal.y * sign, nz = normal.z * sign;
```

No changes to the grid, peak finder, watershed, or assignment methods.

### Stage 1b antipodal merge (hemisphere-normals-density-aware-segment.js)

New block between Stage 1 and Stage 2:

```
For each pair of Lambert normal groups (i, j):
  dot = dot(peak_normal_i, peak_normal_j)
  If dot < -0.85:
    Merge group j into group i
    (move all point indices, clear group j)
```

### Stage 2 mean normal orientation fix

When computing the weighted mean normal for a merged group, point normals
with opposite sense cancel out. Fix: orient each point's normal to agree
with the Lambert peak direction before averaging.

```js
// Before: normals cancel
mx += normalsRaw[no] * w;

// After: orient consistently
if (dot(pointNormal, peakNormal) < 0) flip pointNormal;
mx += nx * w;  // oriented normal
```

## Results

### Dahouet (real-22186-dahouet-502, v7 pipeline)

| Metric | Before | After |
|--------|--------|-------|
| Lambert peaks | 89 | 89 |
| Antipodal groups merged | 0 | 32 |
| Normal groups (after merge) | 89 | 57 (54 with points) |
| Cascade segments | 142 | 158 |

### Synthetic quick set (exploration pipeline)

7/7 passed, avg 91.7 — identical scores, no regression.

## Why coplanar check was wrong

The intuition "only merge if on the same plane" is appealing but fails for
a subtle geometric reason. On a single wall surface, PCA normal orientation
is not random — it correlates with the local point neighborhood structure.
Scan-line LiDAR creates parallel strips of points. Points in strip A (top of
wall) get PCA normal pointing outward; points in strip B (bottom of wall,
different scan angle, slightly different neighborhood geometry) get PCA normal
pointing inward. The two groups' centroids are vertically separated by the
strip spacing (~1m on IGN HD LiDAR), exceeding any reasonable coplanar
threshold.

The correct place for plane separation is **Stage 2 offset KDE**, which
projects individual points (not centroids) onto the normal axis and finds
peaks. Two parallel walls at different offsets produce two KDE peaks and get
split correctly — regardless of whether their normal groups were merged.

## Key insight

The Lambert hemisphere is the right tool for roof normal grouping but
fundamentally cannot handle the **projective plane** topology needed for
unoriented normals. Rather than trying to fix the projection (which always
has a boundary singularity somewhere on the sphere), the pragmatic solution
is: let Lambert do its best, then clean up antipodal artifacts in a
post-processing merge. The merge is simple (dot product check), cheap
(O(k²) where k = number of peaks, typically < 100), and handles all tilt
angles uniformly.
