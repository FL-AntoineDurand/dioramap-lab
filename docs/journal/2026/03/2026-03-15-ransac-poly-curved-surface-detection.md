---
id: "2026-03-15-ransac-poly-curved-surface-detection"
date: 2026-03-15
title: "ransac-poly: polynomial RANSAC for curved surface detection"
tags: [ransac, polynomial, curved-surfaces, density, experiment, v7]
category: experiment
status: concluded
refs:
  - "2026-03-13-v7-master-plan"
  - "2026-03-15-ransac-full-density-compensation"
  - "2026-03-12-local-density-compensation"
pipeline: v7
---

# ransac-poly: polynomial RANSAC for curved surface detection

## Motivation

The v7 master plan (§6.3–6.4) identified curved surface detection as a key unsolved problem. Multiple 10-axis research branches independently discovered techniques for fitting curved primitives (Kasa circle fit, fragment-to-sphere, cone apex regression), but none were integrated into the pipeline. Meanwhile, ransac-full only fits planes — curved roofs (domes, barrel vaults, silos) are shattered into many small planar fragments.

The idea: generalize RANSAC from plane fitting to **degree-2 polynomial surface fitting**. Instead of fitting `ax + by + cz + d = 0` (3 points, perpendicular distance), fit `y = c₀ + c₁u + c₂v + c₃u² + c₄uv + c₅v²` (6 points). This single model naturally captures:
- **Planes** (both curvatures ≈ 0)
- **Cylinders** (one curvature ≈ 0)
- **Domes/spheres** (both curvatures similar)
- **Cones** (both curvatures same sign, different magnitude)
- **Saddles** (opposite-sign curvatures)

Classification falls out from the Hessian eigenvalues of the fitted polynomial — no separate primitive-specific fitter needed.

## Relationship to ransac-full

ransac-poly is a strict **generalization** of ransac-full. A plane is a degree-1 polynomial (c₃=c₄=c₅=0). The key differences:

| Aspect | ransac-full (planes) | ransac-poly (degree-2) |
|---|---|---|
| Sample size | 3 points | 6 points |
| Model | ax + by + cz + d = 0 | y = c₀ + c₁u + c₂v + c₃u² + c₄uv + c₅v² |
| Distance | Perpendicular (implicit eq) | Perpendicular via local normal (explicit eq) |
| Classification | Always "plane" | Hessian eigenvalue analysis |
| Iterations needed | Lower (P = w³ per iter) | Higher (P = w⁶, default 2000 vs 800) |

Both share the same density compensation stack: weighted sampling, weighted scoring, kNN-based connected components, area-based size gates.

## Algorithm

### RANSAC loop

1. Sample 6 distinct points (weighted by per-point area, sparse points more likely)
2. Compute centroid of 6 points (numerical stability — UTM coords are large)
3. Build 6×6 Vandermonde system in centered coordinates, solve via Gaussian elimination with partial pivoting
4. For each remaining point, evaluate polynomial and compute **perpendicular distance** via local surface normal:
   - Vertical residual: `Δy = |y - P(u,v)|`
   - Surface gradient: `∂P/∂u = c₁ + 2c₃u + c₄v`, `∂P/∂v = c₂ + c₄u + 2c₅v`
   - Perpendicular: `d_perp = Δy / √(1 + (∂P/∂u)² + (∂P/∂v)²)`
5. Score = sum of point weights for inliers (d_perp ≤ distThreshold)
6. Keep best-scoring model

### Post-RANSAC per iteration

1. Map inliers to global indices
2. kNN-based connected components (K=15, dist=2.0m floor — bridges scan-line gaps)
3. For each CC with area ≥ minSeedArea:
   - Weighted least-squares polynomial refit (normal equations VᵀWV c = VᵀWy)
   - Plane least-squares fit for comparison
   - Classify via Hessian eigenvalues (gate: poly RMS must improve ≥10% over plane)
   - Compute convex hull in XZ (for frontend mesh rendering)
4. **Give-back**: only remove ASSIGNED points from pool. Rejected CC points return for future iterations. Fallback flush if no seeds produced (prevents infinite loops).

### Perpendicular distance — the tilt bias problem

The polynomial is an **explicit** surface y = f(x,z). The natural residual is vertical (along Y). For a surface tilted at angle θ:

```
d_vertical = d_perpendicular / cos(θ)
```

This inflates the residual for tilted surfaces, effectively shrinking the inlier threshold:
- 0° (flat): threshold = 0.090m (unchanged)
- 35° (gable): threshold effectively 0.074m (82%)
- 45° (steep): 0.064m (71%)
- 60° (mansard): 0.045m (50%)

This creates a systematic bias: horizontal surfaces (ground, flat roofs) capture more inliers and always win the RANSAC competition. Tilted roof planes are starved of inliers and missed entirely.

**Fix (v4)**: compute the per-point perpendicular distance using the local surface normal. For degree-2 polynomials, the normal varies across the surface (unlike planes where it's constant), so this MUST be per-point — a centroid approximation would be wrong for curved surfaces where the normal changes rapidly.

```javascript
const du = c[1] + 2 * c[3] * u + c[4] * v;
const dv = c[2] + c[4] * u + 2 * c[5] * v;
const perpDist = yRes / Math.sqrt(1 + du * du + dv * dv);
```

Cost: 2 extra multiplies + 1 sqrt per point in the inner loop. Negligible compared to the kd-tree operations in CC detection.

### Surface classification

After weighted LS refit, the Hessian of P(u,v):

```
H = [[2c₃, c₄],
     [c₄,  2c₅]]
```

Eigenvalues λ₁, λ₂ (principal curvatures):
- Both |λ| < 0.02 → **plane**
- det(H) < 0 → **saddle** (opposite-sign curvatures)
- |λ_max/λ_min| > 5 → **cylinder** (one curvature dominates)
- |λ₁/λ₂| < 2 → **dome** (both curvatures similar)
- else → **cone** (same sign, different magnitude)

**RMS gate**: if poly RMS improves less than 10% over plane RMS, force classification to "plane" regardless of eigenvalues. This prevents noise-level curvature from triggering false curved classifications.

## Frontend visualization

A new renderer in `app.js` (`renderPolySurfacesArtifact`) builds 3D mesh overlays:
1. For each surface, the backend emits a convex hull in XZ + bounding box
2. Frontend samples the polynomial `y = P(u,v)` on an adaptive grid (5 pts/m, 8–40 resolution)
3. Grid vertices clipped to the convex hull via ray-casting point-in-polygon
4. Triangulated mesh (2 triangles per grid cell) rendered as translucent MeshPhong + wireframe
5. Color by surface type: gray=plane, blue=cylinder, green=dome, orange=cone, purple=saddle
6. Legend shows type counts

## Density compensation (inherited from ransac-full)

All operations from the v7 density compensation stack apply:
- **Weighted sampling**: `ransacPolyWeighted` samples with probability ∝ point weight (Voronoi area)
- **Weighted scoring**: best model = highest total weight (area), not raw count
- **Weighted LS refit**: normal equations weighted by per-point area
- **Area-based size gates**: `minSeedArea` in m², not point count
- **CC connectDist=2.0m**: bridges IGN HD scan-line gaps (was 1.0m, fixed per ransac-full findings)
- **Give-back pool management**: rejected CC points returned to pool (from ransac-full investigation)

## Results

### Synthetic test cases (7 quick, all pass, avg 90.1)

| Test case | Surfaces | Types | Coverage |
|---|---|---|---|
| flat | 2 | 2 plane | 95% |
| gable | 11 | 9 plane, 1 cyl, 1 saddle | 90% |
| hip | 14 | 11 plane, 2 cyl, 1 saddle | 88% |
| dormer | 11 | 6 plane, 4 cyl, 1 saddle | 88% |
| l_shaped | 28 | 17 plane, 9 cyl, 2 saddle | 87% |
| mansard | 11 | 5 plane, 3 cyl, 2 saddle, 1 dome | 91% |
| multi_section | 15 | 9 plane, 5 cyl, 1 saddle | 89% |

### Curved synthetic test cases (21 cases, all pass, avg 70.0)

| Test case | Surfaces | Key detection | RMS improvement |
|---|---|---|---|
| dome-small | 2 | 1 **dome** (R=0.8m) | 92% |
| dome-default | 6 | 2 **dome** | 89-96% |
| cylinder_silo-large | 6 | 1 **cylinder** | 22% |
| silo_with_cone-large | 12 | 3 dome, 2 cone, 1 cyl | 72-100% |
| proc-silo-medium | 6 | 4 **dome** | up to 96% |

Domes detected with 92-96% RMS improvement. Cylinders harder (the `y = P(x,z)` formulation cannot represent horizontal cylinders well — they need axis-aligned parameterization).

### Real test case (Dahouet coastal village)

14 surfaces, 44% assigned. Detects ground surfaces well but tilted roofs still underrepresented. Root cause: coplanar cross-building mixing (same RANSAC limitation as ransac-full, amplified by 6-point sampling probability).

## Remaining limitations

### 1. Explicit surface bias

`y = P(x,z)` cannot represent vertical or near-vertical surfaces. Walls (90°) are mathematically impossible. Very steep surfaces (>70°) have extreme gradient magnitudes, making the polynomial coefficients unstable. This is a fundamental limitation of the height-field parameterization.

**Potential fix**: fit in a **local coordinate frame** (PCA of the 6 sample points) where the surface is approximately horizontal. The polynomial then represents local curvature regardless of global orientation.

### 2. 6-point sampling probability

For inlier ratio w, P(all 6 good) = w⁶. At w=0.3, this is 0.073% — need ~6300 iterations for 99% confidence. Small surfaces in multi-building tiles have very low inlier ratios, making them hard to find even with 2000 iterations.

### 3. Coplanar cross-building mixing

Same as ransac-full: when multiple buildings have similar-height roofs, RANSAC finds a single polynomial that fits points from ALL of them. CC separates them spatially but each fragment is small.

### 4. Cylinder detection weakness

A horizontal cylinder `y = R - √(R² - z²)` requires axis-aligned parameterization. The degree-2 polynomial approximates it as a parabola near the apex but diverges at the sides. Detection works for domes (axis-aligned with Y) but not for horizontal cylinders (barrel vaults, Quonset huts).

## Position in v7 master plan

ransac-poly addresses **§6.3 Convergent techniques** from the v7 master plan. It provides a unified detection framework for the curved primitives that multiple 10-axis research branches identified:

| Master plan technique | ransac-poly coverage |
|---|---|
| Kasa algebraic circle fit | Subsumed — degree-2 poly captures circles as parabolic approximation |
| Fragment-to-sphere detection | Subsumed — dome classification via Hessian eigenvalues |
| Cone apex via linear regression | Partially — cone type detected but apex not extracted |
| Base-plane-constrained hemisphere | Not covered — needs multi-primitive composition |

The step runs as a **dead-end visualization branch** in v7, parallel to ransac-full and planarity-watershed. It does not affect the main pipeline score. Future work could feed classified curved segments into the main path (Stage C/G in the master plan).

## Version history

| Version | Changes |
|---|---|
| v1 | Initial implementation: polynomial RANSAC + classification + frontend mesh renderer |
| v2 | Added convex hull + bbox in surface metadata for frontend rendering |
| v3 | Give-back pool management + connectDist 1.0→2.0m (from ransac-full findings) |
| v4 | Per-point perpendicular distance via local surface normal (fixes tilt bias) |
