---
id: "2026-03-06-next-gen-pipeline-research-axes"
date: 2026-03-06
title: "Next-Generation Pipeline Research Axes — Design Document"
tags: [research, plane-detection, design, ct-radon, multiview, spectral, curvature, pca]
category: design
status: concluded
refs:
  - "2026-03-06-axis1-radon-plane-detection"
  - "2026-03-06-axis2-multiview-rasterize-detect"
  - "2026-03-06-axis3-spectral-frequency-methods"
  - "2026-03-06-axis4-curvature-pca-region-growing"
  - "2026-03-06-four-axis-research-synthesis"
  - "2026-03-06-meta-analysis-pipelines-vs-axes"
---


---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Research Axis 1 — CT/Radon Plane Detection](#2-research-axis-1--ctradon-plane-detection)
3. [Research Axis 2 — Multi-View Rasterize-Detect](#3-research-axis-2--multi-view-rasterize-detect)
4. [Research Axis 3 — Frequency & Spectral Methods](#4-research-axis-3--frequency--spectral-methods)
5. [Research Axis 4 — Normal Convergence (Curved Surfaces)](#5-research-axis-4--normal-convergence-curved-surfaces)
6. [Mathematical Unification](#6-mathematical-unification)
7. [Mapping to Existing Pipeline Steps](#7-mapping-to-existing-pipeline-steps)
8. [Prototype Priority Ranking](#8-prototype-priority-ranking)
9. [References & Inspirations](#9-references--inspirations)
10. [Cross-References to Existing Journal Ideas](#10-cross-references-to-existing-journal-ideas)

---

## 1. Executive Summary

Four research axes emerged from analyzing how industrial CT scanners reconstruct 3D volumes from 2D X-ray projections, and asking: what if we apply the same mathematical frameworks to LiDAR point clouds?

| Axis | Core Idea | Math Field | Finds |
|------|-----------|------------|-------|
| **CT/Radon** | Project point cloud onto 1000 directions, find density peaks | Integral geometry | **Planes** (faces) |
| **Rasterize-Detect** | Project onto 1000 planes, find lines/curves in 2D images | Multi-view geometry | **Edges** (boundaries) |
| **Frequency/Spectral** | Transform to frequency domain (FFT, SH, wavelets, graph Laplacian) | Harmonic analysis | **Multi-resolution structure** |
| **Normal Convergence** | Extend normal-cascade: parallel normals = plane, converging normals = curved surface | Differential geometry | **Curved surfaces** (domes, cylinders, cones) |

These four axes are **not independent** — they are different computational paths to the same underlying geometry, connected through deep mathematical relationships (Section 6). Together they cover faces, edges, curvature, and multi-scale structure — the complete geometric description of a roof.

---

## 2. Research Axis 1 — CT/Radon Plane Detection

### 2.1 Inspiration: How CT Scanning Works

In computed tomography, an X-ray source rotates around an object. At each angle, X-rays pass through the object and a detector captures a 1D intensity profile (how much material the rays traversed). The key steps:

1. **Projection**: X-rays integrate density along their path through the object
2. **Sinogram**: Stack all 1D profiles vs. angle — features trace sine waves
3. **Filtered back-projection**: Apply a ramp filter (boost high frequencies to compensate for low-frequency oversampling), then "smear" each filtered profile back into the reconstruction space
4. **Result**: 3D voxel volume encoding material density

This was formalized by mathematician Johann Radon (Radon transform) and first implemented as a medical CT scan in 1971 by Cormack and Hounsfield (1979 Nobel Prize in Physiology). The mathematical elegance is that filtering is only done in **one dimension**, making it computationally tractable even on 1970s hardware.

### 2.2 The Analogy: LiDAR Point Cloud as "Object"

Instead of X-rays passing through an object, we **project** the point cloud along many directions and analyze the resulting density profiles.

| CT Scanning | LiDAR Adaptation |
|---|---|
| X-ray source + detector | Virtual projection along direction **n** |
| Rotate object physically | Rotate projection direction mathematically |
| Pixel intensity = integrated density | Projected point density = histogram of `dot(p, n)` |
| Sinogram (angle vs position) | Accumulator space (orientation vs distance) |
| Filtered back-projection -> 3D density | Filtered accumulator -> 3D surface reconstruction |

### 2.3 Algorithm

For a direction vector **n** (defined by angles theta, phi on the unit hemisphere), projecting every point gives a scalar:

```
d_i = dot(point_i, n)
```

This produces a **1D density profile** — a histogram of how points distribute along that axis.

**Key insight**: if there is a planar surface in the cloud, all its points share approximately the same `dot(point, normal)` value. When the projection direction **n** aligns with a plane's normal, the histogram shows a **sharp spike**. When misaligned, those points spread out.

#### Full Pipeline

```
1. SAMPLE orientations: ~500-1000 directions on hemisphere (icosphere tessellation)

2. For each direction n:
   a. Project all points: d_i = dot(p_i, n)
   b. Build 1D histogram (bin width ~5 cm)
   c. Apply ramp filter in frequency domain (FFT -> multiply by |f| -> IFFT)

3. ACCUMULATE into (theta, phi, d) space -> 3D "Hough-Radon" accumulator

4. DETECT peaks in accumulator -> each peak = plane parameters (normal n, distance d)
   OR
   Back-project filtered histograms -> 3D density field -> isosurface extraction

5. SEGMENT points to detected planes by proximity to plane equation
```

### 2.4 The Ramp Filter — Why It Matters

Naive back-projection (equivalent to naive Hough voting) oversamples low-frequency structure. Two closely-spaced parallel planes (e.g., a roof slab with thickness, or two floors) blur together. The ramp filter compensates:

- In frequency space, multiply each 1D projection by `|f|` (linearly increasing with frequency)
- This sharpens plane localization — separates closely-spaced parallel planes
- In practice, use a windowed ramp (Shepp-Logan, Hamming) to avoid amplifying noise

This is exactly what CT scanners do, and it's why filtered back-projection produces sharp images from blurry naive back-projection.

### 2.5 Relation to Existing Steps

This approach is a **deterministic, exhaustive alternative to RANSAC seeding**. Where RANSAC randomly samples 3-point planes and hopes to find all surfaces, the CT/Radon approach systematically sweeps all orientations and distances. It also subsumes the normal-histogram-seed step (which is essentially a coarse, unfiltered version of this with manual peak detection).

| Current Step | CT/Radon Equivalent |
|---|---|
| `normal-histogram-seed` | Orientation sweep (theta, phi dimensions of accumulator) |
| `normal-cascade-segment` Stage 2 (offset histogram) | Distance dimension of accumulator |
| `ransac-seed` | Replaced entirely — deterministic peak detection |

### 2.6 Expected Strengths & Weaknesses

**Strengths:**
- Deterministic, exhaustive — never misses a plane orientation
- No random seed dependency
- Naturally handles any number of planes
- The ramp filter provides principled noise rejection
- Could detect very small planes that RANSAC misses (low inlier count)

**Weaknesses:**
- Computational cost: O(N x 1000) projections + 1000 FFTs
- Resolution limited by histogram bin size
- Only finds **planes** — no curved surface support
- Accumulator space is 3D (theta, phi, d) — memory for fine resolution

### 2.7 Complexity Estimate

For N = 50,000 points, 1000 directions, 500-bin histograms:
- Projection: 50M dot products (trivial, ~50ms)
- FFT + filter: 1000 x 512-point FFTs (~1ms)
- Peak detection: 3D accumulator ~500M cells at fine resolution, but most are sparse
- **Total estimate: 100-500ms** — competitive with RANSAC

---

## 3. Research Axis 2 — Multi-View Rasterize-Detect

### 3.1 Core Idea

Instead of projecting to 1D (like CT), project the point cloud onto 2D planes and look for **geometric features** (lines, curves) in the resulting raster images.

```
For each of ~1000 projection planes:
  1. RASTERIZE: orthographic projection of 3D points -> 2D density image
  2. DETECT: find lines, curves, alignments in the 2D image
  3. REFINE: small rotation/translation of the plane to maximize
     sharpness (thinnest, most intense features)
  4. SEGMENT: extract edges, fit Bezier curves
  5. REPROJECT: lift 2D segments back into 3D
  6. ASSEMBLE: merge 3D curves from all planes into a wireframe/mesh
```

### 3.2 Why This Differs from CT/Radon

| | CT/Radon (Axis 1) | Rasterize-Detect (Axis 2) |
|---|---|---|
| **Projection** | 3D -> **1D** (dot product along direction) | 3D -> **2D** (orthographic onto plane) |
| **What you look for** | Density peaks (= planes exist here) | Geometric features (lines, curves) |
| **What it finds** | **Planes** (orientation + distance) | **Edges** (ridgelines, eaves, boundaries) |
| **Reconstruction** | Density field -> isosurface | Wireframe -> mesh |
| **Optimization** | None (exhaustive sampling + filter) | Iterative refinement per plane |
| **Output** | Surfaces (filled faces) | Skeleton (edges, curves) -> then fill |

**CT finds faces. Rasterize-Detect finds edges. They are dual representations.**

### 3.3 The "Auto-Focus" Insight

When projecting a roof's ridge line onto a plane **perpendicular** to it, the line collapses to a single bright point. When projecting onto a plane **containing** the ridge, it appears as a **thin sharp line**. Any other angle produces a thick, smeared band.

The optimization step — "rotate the plane to maximize line thinness" — is essentially **finding the plane that contains the edge**. Two such planes from different viewing angles fully constrain the edge's 3D position. This is **epipolar geometry applied to point clouds**.

### 3.4 What Lines Appear on the 2D Raster

**Plan view (looking down):**
```
    /\          <- ridge line
   /  \         <- hip lines
  /    \
 /______\       <- eave lines
```

**Elevation view (from the side):**
```
  /\
 /  \___        <- roof slope profile, height steps
|      |        <- wall lines
```

Each view reveals **different edges**. Combining many views gives the complete edge skeleton.

### 3.5 Bezier Fitting for Curved Geometry

This is where Axis 2 uniquely shines. By fitting Bezier/B-spline curves instead of only line segments, we can reconstruct curved geometry that plane-based methods fundamentally cannot handle:

- Turrets, domes, barrel vaults -> circular/elliptical curves
- Mansard transitions -> compound curves
- Organic shapes (thatched roofs, curved modern architecture)

### 3.6 The Reassembly Problem

The hardest part: thousands of 2D curve segments from hundreds of views must be matched and merged into a consistent 3D wireframe. This is **multi-view stereo for edges**:

- Same 3D edge detected in many views -> correspondence problem
- Conflicting detections from noise and occlusion
- Need topological consistency (edges meet at vertices, faces are closed)

### 3.7 Relation to Existing Steps

| Current Step | Rasterize-Detect Equivalent |
|---|---|
| `hull-extraction` | Edge detection from plan-view projection |
| `parallel-edge-clip` | Ridge/hip line detection from optimally-oriented views |
| `sharp-edges` | Bezier/polyline fitting of detected features |
| `clip-to-footprint` | Eave line detection from elevation views |

### 3.8 Hybrid with CT/Radon

The two axes combine naturally:

```
Phase 1: CT/Radon (global, fast)
  -> Dominant plane orientations + distances
  -> Gives roof facets as infinite planes

Phase 2: Rasterize-Detect (targeted, precise)
  -> Project onto each detected plane's own coordinate system
  -> Now looking "along the face" — edges appear as sharp lines
  -> Segment with Bezier/polyline fitting
  -> These are the face boundaries

Phase 3: Assemble
  -> Clip each plane to its detected boundary curves
  -> Intersect adjacent faces at ridges/hips
  -> Close the model (add walls, ground plane)
```

Phase 1 tells you **what planes exist**. Phase 2 tells you **where each plane ends**. Phase 3 snaps it all together.

---

## 4. Research Axis 3 — Frequency & Spectral Methods

### 4.1 Overview

A roof is a **low-frequency object** — a few flat planes, maybe some gentle curves. Noise, sensor jitter, and small features are high-frequency. Frequency-domain methods are precisely built to separate these.

Six sub-approaches span this axis, from direct DSM image processing to abstract spectral geometry on the point cloud graph.

### 4.2 Sub-Axis 3A: FFT/DCT on the DSM (Height Map)

The pipeline already rasterizes point clouds into DSMs (Digital Surface Models). A DSM is a grayscale image — every image-processing frequency technique applies directly.

#### DCT Block Analysis (the JPEG Transform)

JPEG divides images into 8x8 blocks and applies the Discrete Cosine Transform. Most energy concentrates in a few low-frequency coefficients. Applied to DSM blocks:

```
For each 8x8 block of the DSM grid:
  Apply 2D DCT
  Analyze coefficient distribution:
    DC only (one flat coefficient)       -> flat plane region
    DC + one gradient direction          -> tilted plane
    Significant high-freq energy         -> edge / ridge / complex zone
    Periodic pattern                     -> roof tiles / solar panels
```

**This gives a free segmentation into "simple" vs "complex" zones.** Route simple blocks to fast plane fitting, spend compute on complex blocks. This is **adaptive level-of-detail for processing** — a meta-step that makes the entire pipeline smarter about resource allocation.

#### 2D FFT on Full DSM

```
2D FFT of DSM ->
  Strong peaks at specific frequencies   = periodic structures (tile rows, panels)
  Low-freq dominance                     = smooth roof, easily modeled
  Broad high-freq energy                 = noisy data, needs denoising
  Directional energy concentration       = reveals dominant slope orientations
```

**Practical application — FFT denoising**: Zero out high-frequency components, inverse FFT -> smoothed DSM preserving plane geometry while removing sensor noise. More principled than Gaussian smoothing, which blurs edges.

#### Phase Correlation (Already Used)

The cross-correlation alignment in `lib/image/ortho-align.js` is already a frequency-domain method — cross-correlation in spatial domain equals multiplication in frequency domain. This same technique could extend to **aligning overlapping flight strips** before merging.

### 4.3 Sub-Axis 3B: Spherical Harmonics — Fourier on the Sphere

The normal-cascade pipeline starts with a **normal histogram** on the Gaussian sphere. That histogram is a function on S^2, and the Fourier transform for functions on a sphere is **Spherical Harmonics (SH)**.

```
Current approach:
  Bin normals into 2D histogram -> find peaks -> seed planes

SH approach:
  Compute SH coefficients of the normal distribution
  L=0: isotropic component (overall "spread")
  L=1: dominant direction (mean normal)
  L=2: dominant planes (quadrupolar structure)
  L=3+: finer angular features

  Truncate at low L = robust dominant orientation finding
  No binning artifacts, no bin size parameter
  Rotation-invariant representation
```

**Why this matters**: The current histogram binning (`normalTiltBin`, `normalAzBin` params in `normal-histogram-seed` and `normal-cascade-segment`) introduces a resolution/sensitivity tradeoff — bin too coarse and closely-oriented planes merge, bin too fine and noise creates false peaks. SH decomposition gives **continuous, multi-resolution angular analysis** with no binning at all.

For a typical roof:
- 2 dominant planes -> L=2 is enough (5 coefficients)
- 4 hip planes -> L=3-4 (~25 coefficients)
- Complex multi-facet -> L=6-8 (~80 coefficients)
- Still vastly fewer parameters than a histogram with thousands of bins

### 4.4 Sub-Axis 3C: Graph Laplacian — Spectral Geometry on the Point Cloud

The most powerful and least obvious sub-axis. Work directly on the **point cloud as a graph** without rasterizing.

#### Build the Graph

```
Each point = node
Connect k-nearest neighbors (k=15-20)
Edge weight = exp(-dist^2 / sigma^2)  (Gaussian kernel)
Compute graph Laplacian matrix L = D - W
  where D = degree matrix, W = weighted adjacency matrix
```

#### Eigenvectors = "Vibration Modes" of the Surface

```
Eigendecompose L -> eigenvalues lambda_1 <= lambda_2 <= ... and eigenvectors v_1, v_2, ...

v_1 (lambda=0): constant, trivial
v_2, v_3: smoothest partitions of the surface (Fiedler vector)
  -> SPECTRAL SEGMENTATION: sign(v_2) splits the cloud into 2 regions
  -> Naturally cuts along edges/ridges where geometry changes abruptly
Low eigenvectors: large-scale shape components
High eigenvectors: fine details, edges
```

**This is segmentation without any plane model.** The graph Laplacian "knows" where the surface bends — eigencuts happen at ridges, edges, and discontinuities. No RANSAC, no normals, no region growing, no seeds. Pure spectral geometry.

#### Automatic Cluster Count via Eigenvalue Gap

```
Use eigenvalues lambda_2 through lambda_5  -> 2-5 roof facets segmented
Use eigenvalues lambda_2 through lambda_20 -> dormers, chimneys isolated

Large gap between lambda_k and lambda_{k+1} -> "k natural clusters"
```

A simple hip roof: gap after lambda_5 (4 faces + ground). A complex roof with dormers: gap after lambda_12. **The eigenvalue spectrum itself encodes the complexity of the roof.**

#### Relation to Curvature (Connection to Axis 4)

The graph Laplacian is a discrete approximation of the **Laplace-Beltrami operator** on the surface. The Laplace-Beltrami operator's eigenvalues encode curvature information. So spectral segmentation and curvature-based classification (Axis 4) are mathematically the same thing, accessed through different computational paths.

### 4.5 Sub-Axis 3D: Wavelet Transform — Multi-Resolution Edge Detection

Wavelets provide frequency analysis **localized in space** (FFT tells you "what frequencies exist" but not "where"). For edge detection on DSMs:

```
2D Wavelet decomposition of DSM (e.g., Daubechies-4):
  Level 1 (fine):    sensor noise, small artifacts   -> threshold/remove
  Level 2 (medium):  ridge lines, hip lines, wall edges -> DETECT THESE
  Level 3 (coarse):  overall roof slope changes       -> plane boundaries
```

**Edge-aware denoising** (vastly superior to Gaussian smoothing):

```
1. Wavelet decompose the DSM
2. At each level, separate coefficients:
   Large magnitude = real geometric feature  -> keep
   Small magnitude = noise                   -> zero out (soft thresholding)
3. Inverse wavelet -> denoised DSM with sharp edges preserved
```

Gaussian smoothing blurs edges. Wavelet denoising preserves edges while removing noise — exactly what the pipeline needs before plane fitting.

### 4.6 Sub-Axis 3E: 3D Fourier Transform — Theoretical Nuclear Option

Represent the point cloud as a 3D density function (voxelize or Gaussian splats), then take the 3D FFT.

**A plane in spatial domain = a line in frequency domain:**

```
Spatial:   plane with normal n at distance d
           f(x) = delta(dot(x, n) - d)

Frequency: line through origin along n
           F(k) = e^{-2*pi*i * d * |k|}  *  delta(k x n)
```

So **finding planes = finding lines in 3D Fourier space**. This is the **exact dual of the Hough transform**.

The **Fourier Slice Theorem** connects this to the CT approach: a 1D projection's FT equals a line through the 3D FT. All three approaches (CT projections, 2D rasterization, 3D FFT) are different slices through the same mathematical object.

In practice, the 3D FFT on a voxel grid is expensive, but theoretically elegant — it's the unifying framework.

### 4.7 Summary Table — Frequency Methods

| Method | Input | Pipeline Step It Could Replace/Improve | Impact |
|---|---|---|---|
| DCT block analysis | DSM raster | New meta-step: adaptive complexity routing | Medium |
| FFT denoising | DSM raster | `extract-denoise` (edge-preserving) | High |
| Spherical Harmonics | Normal vectors | `normal-histogram-seed`, `normal-cascade-segment` Stage 1 | High |
| Graph Laplacian | Point cloud graph | `competitive-grow`, region segmentation | Very High |
| Wavelet denoising | DSM raster | `extract-denoise` (multi-scale, edge-preserving) | High |
| 3D FFT plane detection | Voxelized cloud | `ransac-seed` (deterministic) | Medium |
| Phase correlation | DSM/ortho images | `co-register` (already used) | Already done |

---

## 5. Research Axis 4 — Normal Convergence (Curved Surfaces)

### 5.1 Motivation

The current normal-cascade detects planes by finding points whose normals are **parallel** (equal orientation). But many real roof features are curved: turrets, barrel vaults, domes, conical towers, Haussmann zinc caps, church apses.

All of these have a simple geometric signature: their **anti-normal rays converge**.

### 5.2 The Anti-Normal Ray

For each point with position **p** and outward normal **n**, the anti-normal ray shoots inward:

```
ray(t) = p - t * n    (t > 0, pointing inward from surface)
```

For a plane, anti-normal rays are all parallel — they never meet.
For a curved surface, anti-normal rays converge toward the **center of curvature**.

### 5.3 Surface Classification by Convergence Pattern

| Surface | Principal Curvatures | Anti-Normal Convergence | Locus |
|---|---|---|---|
| **Plane** | kappa_1 = kappa_2 = 0 | Parallel (never meet) | Point at infinity |
| **Cylinder** | kappa_1 = 0, kappa_2 = 1/r | Converge to a **line** (axis) | 1D axis |
| **Cone** | kappa_1 = 0, kappa_2 = varies | Converge to a **point** (apex) | 0D point |
| **Sphere / Dome** | kappa_1 = kappa_2 = 1/r | All converge to **center**, equal distance | 0D point + constant radius |
| **Ogive** (pointed arch) | kappa_1 = kappa_2, two radii | Converge to **two centers** | Two clusters |
| **Saddle** | kappa_1 > 0, kappa_2 < 0 | Diverge in one direction, converge in other | Hyperbolic |

### 5.4 Algorithm

```
Given a set of points with normals (e.g., residual points after plane detection):

Step 1 — PAIRWISE CLOSEST APPROACH
  For sampled pairs of anti-normal rays (p_i - t*n_i) and (p_j - s*n_j):
    Compute closest approach point C_ij and distance d_ij
    (Closed-form solution — 3 lines of linear algebra)

Step 2 — CLUSTER convergence points
    Tight cluster at a point   -> sphere, dome, or cone apex
    Linear cluster (along a line) -> cylinder axis
    Two clusters               -> ogive (two-center arch)
    No cluster                 -> freeform surface (bail out)

Step 3 — CLASSIFY by distance distribution
  Given convergence locus L:
    dist(p_i, L) all equal              -> sphere (radius = that distance)
    dist(p_i, L) varies linearly        -> cone (opening angle = slope)
    dist(p_i, L) all equal + L is line  -> cylinder (radius = that distance)
    dist(p_i, L) follows an arc         -> ogive (fit generating curve)

Step 4 — FIT parameters
    Sphere:   center + radius                    (4 params)
    Cylinder: axis point + axis direction + radius (5 params)
    Cone:     apex + axis direction + half-angle   (6 params)
    Ogive:    two centers + two radii             (compact parameterization)
```

### 5.5 Closest Approach of Two Rays — The Core Computation

```javascript
function closestApproach(p1, n1, p2, n2) {
  const d = vec3.sub(p2, p1);
  const n1n2 = vec3.dot(n1, n2);
  const denom = 1 - n1n2 * n1n2;
  if (Math.abs(denom) < 1e-10) return null; // parallel = plane, skip
  const t = (vec3.dot(d, n1) - n1n2 * vec3.dot(d, n2)) / denom;
  const s = (n1n2 * vec3.dot(d, n1) - vec3.dot(d, n2)) / denom;
  const c1 = vec3.add(p1, vec3.scale(n1, -t));
  const c2 = vec3.add(p2, vec3.scale(n2, -s));
  return {
    midpoint: vec3.scale(vec3.add(c1, c2), 0.5),
    distance: vec3.length(vec3.sub(c1, c2))
  };
}
```

This is trivial to compute. Random sampling of ~200 pairs from residual points is sufficient to detect convergence. Refine with all inliers once the pattern is identified.

### 5.6 Integration with Normal-Cascade

```
Existing pipeline:
  1. Estimate normals
  2. Normal histogram -> dominant plane orientations
  3. Seed planes, grow regions
  4. Residual points = "unassigned"    <-- THESE ARE YOUR CURVED SURFACES

New addition:
  5. Take residual points
  6. Sample pairs -> compute convergence points
  7. Cluster convergence points (DBSCAN or simple grid)
  8. Classify: sphere / cylinder / cone / ogive
  9. Fit parametric surface
  10. Assign inlier points
  11. Remaining residuals -> truly freeform or noise
```

This is a **post-plane cleanup step**. Planes eat the easy surfaces, then convergence detection catches the curved leftovers. Minimal disruption to the existing pipeline.

### 5.7 Real Roof Feature Examples

| Roof Feature | Surface Type | Normal Convergence |
|---|---|---|
| Turret / tower cap | Cone or dome | Point convergence above apex |
| Barrel vault | Cylinder | Axis convergence along ridge |
| Rounded dormer | Half-cylinder or dome | Axis or center below ridge |
| Church apse | Half-dome | Center convergence behind wall |
| Haussmann zinc dome | Ogive | Two-center convergence |
| Conical tower (chateau) | Cone | Apex above peak |
| Modern curved roof | Cylinder / freeform | Axis if cylindrical |
| Agricultural silo | Cylinder + dome cap | Axis (walls) + center (cap) |

### 5.8 The Duality with Plane Detection

The normal-cascade asks: **"are your normals equal?"** -> `dot(n_1, n_2) ~ 1`

Normal convergence asks: **"do your anti-normals meet?"** -> `closestApproach.distance ~ 0`

Same data, dual question. Both are discrete approximations of the **shape operator** (second fundamental form) of the surface — planes have zero curvature, curved surfaces have nonzero curvature. See Section 6 for the full mathematical unification.

---

## 6. Mathematical Unification

The four research axes appear different computationally, but they are connected through deep mathematical structures. Understanding these connections reveals that they are **complementary projections of the same geometry**, not competing approaches.

### 6.1 The Two Pillars

Everything reduces to two dual viewpoints of the same geometry:

| | **Local (Differential)** | **Global (Integral)** |
|---|---|---|
| **What it measures** | Curvature at each point | Density along projections |
| **Math framework** | Second fundamental form, shape operator | Radon transform, X-ray transform |
| **Discrete proxy** | Normal comparison (cascade, convergence) | Histogram of projections (CT, rasterize) |
| **Frequency dual** | Graph Laplacian eigenvectors, SH of Gauss map | 3D FFT, Fourier Slice Theorem |
| **Finds** | Surface type (plane, cylinder, sphere...) | Surface position and extent |
| **Strength** | Classification | Localization |

**Local tells you WHAT it is. Global tells you WHERE it is.** You need both.

### 6.2 Unification: Normal Cascade + Normal Convergence = Curvature

Both are discrete approximations of the **principal curvatures** (eigenvalues of the shape operator):

| Surface | kappa_1 | kappa_2 | Normal Test |
|---|---|---|---|
| Plane | 0 | 0 | Parallel (no convergence) |
| Cylinder | 0 | 1/r | Converge to axis (one direction) |
| Cone | 0 | varies | Converge to apex |
| Sphere | 1/r | 1/r | Converge to center (all directions) |
| Saddle | +kappa | -kappa | Diverge in one dir, converge in other |

The fully general version is a **unified surface detector**:

```
For each point, estimate the 2x2 shape operator S (from local PCA):
  eigenvalues(S) -> (kappa_1, kappa_2)

  kappa_1 ~ 0, kappa_2 ~ 0         -> plane
  kappa_1 ~ 0, kappa_2 ~ const     -> cylinder
  kappa_1 ~ kappa_2 ~ const != 0   -> sphere
  kappa_1 ~ 0, kappa_2 varies      -> cone (or developable surface)
  kappa_1 ~ kappa_2, varies        -> general dome
  kappa_1 * kappa_2 < 0            -> saddle / hyperbolic
```

**The normal-cascade already computes half of this and discards the other half.** PCA for normal estimation produces three eigenvalues — the smallest gives the normal direction, but the **ratio of the two larger eigenvalues encodes curvature**. Currently this information is thrown away.

### 6.3 Unification: CT/Radon + Rasterize-Detect = Integral Geometry

Both are instances of the **generalized Radon transform** at different dimensions:

```
R_k: integrate a function over k-dimensional subspaces

k=1 (lines):  Line integrals -> CT approach (Axis 1)
               3D -> project along lines -> 1D profiles parameterized by (theta, phi, d)

k=2 (planes): Plane integrals -> Rasterize-Detect approach (Axis 2)
               3D -> project onto planes -> 2D images parameterized by (theta, phi)
```

The **Fourier Slice Theorem** connects them to frequency space:

```
1D projection of 3D object  ->  1D FT = line through 3D FT
2D projection of 3D object  ->  2D FT = plane through 3D FT
```

So the CT approach, the rasterize-detect approach, and the 3D FFT approach are **three views of the same mathematical object** — the 3D Fourier transform of the point cloud's density function.

### 6.4 The Gauss Map Unifies Normal Analysis + Spherical Harmonics

The **Gauss map** sends each surface point to its normal on the unit sphere S^2. The distribution of normals on S^2 is a function that encodes everything about surface orientations:

```
Surface -> Gauss map -> function on S^2 -> Spherical Harmonics decomposition

Plane:     all normals -> single point on S^2   -> delta function
Cylinder:  normals -> great circle on S^2       -> band-limited in SH
Sphere:    normals -> uniform on S^2            -> only L=0 (isotropic)
```

So:
- **Normal histogram** = discretized Gauss map
- **Spherical Harmonics** = Fourier analysis of the Gauss map
- **Normal cascade** = peak detection on the Gauss map
- **Normal convergence** = analyzing the *spread pattern* around peaks on the Gauss map

**All four are different ways to read the same mathematical object.**

### 6.5 The Grand Unification Diagram

```
                     DIFFERENTIAL GEOMETRY
                     (Second Fundamental Form)
                            |
               +------------+------------+
               |            |            |
          kappa=0,0    kappa=0,1/r   kappa=1/r,1/r
           PLANE        CYLINDER       SPHERE
               |            |            |
               +------------+------------+
                            |
                       Gauss Map
                     (normals on S^2)
                            |
               +------------+------------+
               |            |            |
        Normal Histogram  Spherical    Normal
        (discrete bins)   Harmonics    Convergence
               |          (continuous)  (ray intersection)
               |            |            |
               +------------+------------+
                            |
                     Function on S^2
                     (parameterized by direction)
                            |
           +----------------+----------------+
           |                |                |
     Radon Transform   3D Fourier       Graph Laplacian
     (integrate along   Transform       (discrete Laplace-
      directions)       (global)         Beltrami)
           |                |                |
      +----+----+     Fourier Slice    Eigenvalues =
      |         |      Theorem          curvature encoding
   1D proj   2D proj       |                |
   (CT)    (rasterize)     |           Spectral
      |         |          |           Segmentation
      +----+----+          |                |
           |               |                |
           +---------------+----------------+
                           |
                    HARMONIC ANALYSIS
                    (Fourier / Wavelets / SH)
```

### 6.6 Practical Implication

Instead of four separate algorithms, the mathematical unification suggests a **single generalized step**:

```
UnifiedSurfaceDetector:
  1. Compute shape operator at each point -> (kappa_1, kappa_2) per point
     (from local PCA — already partially computed for normal estimation)

  2. Cluster points by curvature signature:
     (0, 0) cluster       -> route to plane fitting (existing cascade)
     (0, c) cluster       -> route to cylinder fitting
     (c, c) cluster       -> route to sphere/dome fitting
     (0, varying)         -> route to cone fitting
     mixed/complex        -> route to spectral/frequency methods

  3. For each cluster, fit the appropriate parametric surface

  4. Residuals -> wavelet denoise -> retry or flag as freeform
```

The shape operator subsumes both normal-equality and normal-convergence as special cases. And it's already half-computed during normal estimation.

---

## 7. Mapping to Existing Pipeline Steps

### 7.1 Where Each Research Axis Fits

```
EXISTING PIPELINE (v6a-base):

extract-denoise .............. [3D: wavelet denoise] [3A: DCT adaptive routing]
    |
normal-cascade-segment ....... [3B: SH replaces histogram] [4: curvature classification]
    |                          [1: CT/Radon replaces Stage 1+2]
    |
ransac-seed .................. [1: CT/Radon accumulator replaces RANSAC entirely]
    |
competitive-grow ............. [3C: graph Laplacian spectral segmentation]
    |
point-stealing ............... [3C: spectral boundary refinement]
    |
refit-planes ................. (unchanged)
    |
hull-extraction .............. [2: rasterize-detect for precise boundaries]
    |
parallel-edge-clip ........... [2: multi-view edge detection]
    |
sharp-edges .................. [2: Bezier fitting from multi-view curves]
    |
close-corner-gaps ............ (unchanged)
    |
clip-to-footprint ............ (unchanged)
    |
obstacle-filter .............. [4: curved surface detection catches false "obstacles"]
    |
resolve-overlaps ............. (unchanged)
    |
shape-fitting ................ [4: cylinder/cone/dome fitting for non-planar roofs]
```

### 7.2 New Steps to Create

| Proposed Step | Research Axis | Replaces/Augments | Priority |
|---|---|---|---|
| `wavelet-denoise` | 3D | `extract-denoise` (denoising part) | HIGH |
| `sh-normal-analysis` | 3B | `normal-cascade-segment` Stage 1 | HIGH |
| `spectral-segment` | 3C | `competitive-grow` | VERY HIGH |
| `radon-plane-detect` | 1 | `ransac-seed` | MEDIUM |
| `curvature-classify` | 4 + 6 | New (post-plane) | HIGH |
| `convergence-fit` | 4 | New (post-curvature-classify) | MEDIUM |
| `multiview-edge-detect` | 2 | `hull-extraction` + `parallel-edge-clip` | MEDIUM |
| `dct-complexity-router` | 3A | New (meta-step, adaptive routing) | LOW |
| `bezier-edge-fit` | 2 | `sharp-edges` | LOW |

### 7.3 Possible Next-Gen Pipeline

```
extract-denoise  (with wavelet denoising option)
    |
dct-complexity-router  (classify regions: simple/complex)
    |
    +-- simple regions -----> radon-plane-detect (fast, deterministic)
    |
    +-- complex regions ----> sh-normal-analysis
    |                             |
    |                         curvature-classify
    |                             |
    |                    +--------+--------+
    |                    |                 |
    |               kappa ~ 0          kappa != 0
    |            (planes)           (curved surfaces)
    |                    |                 |
    |            spectral-segment    convergence-fit
    |                    |                 |
    |                    +--------+--------+
    |                             |
    +-----------------------------+
                |
          refit-surfaces  (unified plane + curved surface refit)
                |
          multiview-edge-detect  (precise boundaries)
                |
          bezier-edge-fit  (curved edge segments)
                |
          close-corner-gaps
                |
          clip-to-footprint
                |
          resolve-overlaps
                |
          shape-fitting  (extended for curved primitives)
```

---

## 8. Prototype Priority Ranking

Ranked by **impact/effort ratio** for the roof reconstruction benchmark:

### Tier 1 — High Impact, Moderate Effort

| # | Prototype | Axis | Why First | Effort |
|---|-----------|------|-----------|--------|
| 1 | **Wavelet denoising** | 3D | Drop-in replacement for Gaussian smoothing. Immediate quality improvement. Well-understood algorithms, npm packages exist (`ndarray-fft`, custom wavelet). Edge-preserving = better plane fitting downstream. | ~2 days |
| 2 | **Graph Laplacian segmentation** | 3C | Could revolutionize segmentation. Replaces heuristic region growing with mathematically optimal spectral clustering. No seeds, no grow parameters. Eigenvalue gap gives automatic cluster count. | ~3 days |
| 3 | **Curvature classification (unified shape operator)** | 4 + 6 | Already half-computed during normal estimation — just need to keep the eigenvalue ratio instead of discarding it. Enables curved surface detection with minimal new code. | ~2 days |

### Tier 2 — High Impact, Higher Effort

| # | Prototype | Axis | Why | Effort |
|---|-----------|------|-----|--------|
| 4 | **SH normal analysis** | 3B | Cleaner than histogram, eliminates bin-size parameter. But current histogram works well — improvement is incremental. SH library needed. | ~3 days |
| 5 | **CT/Radon plane detection** | 1 | Deterministic, exhaustive, elegant. But current RANSAC + cascade already works well for planes. Main win is on roofs with many small facets. | ~3 days |
| 6 | **Normal convergence fitting** | 4 | Depends on #3 (curvature classification) to identify curved regions first. Then fits parametric surfaces. Impact limited to buildings with curved features. | ~2 days |

### Tier 3 — Speculative, Research-Heavy

| # | Prototype | Axis | Why Later | Effort |
|---|-----------|------|-----------|--------|
| 7 | **Multi-view edge detection** | 2 | The reassembly problem (matching 2D features across views) is research-hard. High payoff if solved, but high risk. | ~5 days |
| 8 | **DCT adaptive routing** | 3A | Meta-optimization — only useful once we have multiple pipeline paths to route between. Build the paths first. | ~1 day |
| 9 | **3D FFT plane detection** | 3E | Theoretically beautiful, but the voxelization overhead and memory cost may not beat the CT/Radon approach. Mainly useful as a unifying framework for understanding. | ~4 days |

### Recommended First Sprint

```
Sprint 1 (1 week):
  - Prototype #1 (wavelet denoise) as a new step
  - Prototype #3 (curvature classify) by extending normal estimation
  - Benchmark both against v6a-base on 201 test cases

Sprint 2 (1 week):
  - Prototype #2 (graph Laplacian segmentation) as alternative to competitive-grow
  - Prototype #6 (convergence fitting) building on #3
  - Benchmark new pipeline variants

Sprint 3 (1 week):
  - Prototype #5 (CT/Radon) as alternative to RANSAC seeding
  - Prototype #4 (SH normals) as alternative to histogram
  - Compare all new approaches head-to-head
```

---

## 9. References & Inspirations

### 9.1 Origin

This research document originated from watching a Scott Manley video about Lumafield industrial CT scanners, which explained the Radon transform and filtered back-projection. The question "what if we apply this to LiDAR point clouds?" led to a cascade of connected ideas spanning integral geometry, differential geometry, spectral methods, and multi-view feature detection.

### 9.2 Mathematical Foundations

| Concept | Field | Key Result | Relevance |
|---|---|---|---|
| Radon Transform | Integral geometry | Encodes a function via its line integrals | Axis 1 — CT plane detection |
| Fourier Slice Theorem | Harmonic analysis | 1D projection's FT = slice of nD FT | Unifies Axes 1, 2, 3E |
| Filtered Back-Projection | Inverse problems | Ramp filter compensates low-freq oversampling | Axis 1 — sharpening |
| Spherical Harmonics | Harmonic analysis on S^2 | Fourier transform for functions on the sphere | Axis 3B — normal analysis |
| Graph Laplacian | Spectral graph theory | Eigenvectors = optimal graph partitions | Axis 3C — segmentation |
| Laplace-Beltrami Operator | Differential geometry | Surface "vibration modes" encode curvature | Connection 3C <-> 4 |
| Second Fundamental Form | Differential geometry | Shape operator eigenvalues = principal curvatures | Axis 4 — unified classification |
| Gauss Map | Differential geometry | Surface -> unit sphere via normals | Unifies normal cascade + SH |
| Hough Transform | Computer vision | Dual of Radon transform for feature detection | Axis 1 (related) |
| Wavelet Transform | Time-frequency analysis | Localized frequency decomposition | Axis 3D — edge-preserving denoise |
| DCT (Discrete Cosine Transform) | Signal processing | Energy compaction in low frequencies | Axis 3A — complexity routing |

### 9.3 Related Work in the Codebase

| Existing | File | Connection |
|---|---|---|
| Normal cascade (V6) | `lib/pipeline/steps/normal-cascade-segment.js` | Axes 3B, 4 extend this |
| Cross-correlation alignment | `lib/image/ortho-align.js` | Already uses frequency-domain methods (Axis 3) |
| DSM rasterization | `lib/pipeline/steps/rasterize-dsm.js` (if exists) | Input for Axes 3A, 3D |
| Normal estimation (PCA) | Within cascade steps | Already computes half of Axis 4's shape operator |
| Sensor-aware params | `lib/pipeline/sensor-params.js` | Threshold derivation applies to new steps |

### 9.4 Potential npm Packages

| Package | Purpose | Axis |
|---|---|---|
| `ndarray-fft` | FFT on N-dimensional arrays | 3A, 3E |
| `spherical-harmonics` | SH computation | 3B |
| `ml-matrix` | Eigendecomposition for graph Laplacian | 3C |
| `discrete-wavelets` | 1D/2D wavelet transforms | 3D |
| `dbscan` | Clustering convergence points | 4 |

---

---

## 10. Cross-References to Existing Journal Ideas

Several previously proposed journal ideas directly connect to or would be subsumed by these research axes. This section maps them to avoid duplicate work and highlight synergies.

### Axis 4 (Normal Convergence / Curved Surfaces)

| Journal Entry | Connection |
|---|---|
| [`2026-02-26-idea-cylindrical-detection-silos`](journal/2026/02/2026-02-26-idea-cylindrical-detection-silos.md) | **Direct match.** Proposed cylindrical surface detection for silos. Axis 4 generalizes this to all quadric surfaces (cylinders, cones, spheres, domes). The normal convergence algorithm detects cylinders as a special case (anti-normals converging to an axis). This idea is now a subset of the unified curvature classifier (Section 5 + Section 6.2). |
| [`2026-02-26-idea-bdtopo-metadata-priors`](journal/2026/02/2026-02-26-idea-bdtopo-metadata-priors.md) | **Synergy.** Proposed using BDTOPO `nature='Silo'` to route to cylindrical detection. With Axis 4, the curvature classifier can auto-detect silos without metadata priors — but BDTOPO priors could still accelerate detection by skipping the plane-fitting phase entirely for known silos. The two approaches are complementary: BDTOPO says "expect a cylinder," curvature confirms it from data. |

### Axis 2 (Multi-View Rasterize-Detect) & Axis 3D (Wavelet Edge Detection)

| Journal Entry | Connection |
|---|---|
| [`2026-02-26-idea-ortho-ridge-confirmation`](journal/2026/02/2026-02-26-idea-ortho-ridge-confirmation.md) | **Related.** Proposed Sobel/Canny edge detection on orthophotos to confirm LiDAR ridges. Axis 2 generalizes this: instead of only confirming ridges from ortho, project the point cloud itself onto multiple planes and detect edges directly. The ortho ridge confirmation is a specific instance where one particular view (plan view) is used with an external image source. Both could coexist: Axis 2 for LiDAR-intrinsic edge detection, ortho confirmation for cross-modal validation. |
| [`2026-02-26-idea-ridge-detection-validation`](journal/2026/02/2026-02-26-idea-ridge-detection-validation.md) | **Subsumed.** Proposed Hessian eigendecomposition to validate plane boundaries. The Hessian eigenvalues (`lambda2/lambda1` ridge strength) are local curvature measures — a 2D version of the shape operator from Section 6.2. Axis 4's curvature classifier subsumes this: strong curvature along a plane boundary = confirmed ridge, weak curvature = possible false split. Status: abandoned, but the math maps directly to the unified framework. |

### Axis 3D (Wavelet Denoising) & Axis 3A (DCT Complexity Routing)

| Journal Entry | Connection |
|---|---|
| [`2026-03-05-idea-denoise-ablation`](journal/2026/03/2026-03-05-idea-denoise-ablation.md) | **Motivating evidence.** Shows current grid-based denoising kills ~80% of multi-return edge points. Wavelet denoising (Axis 3D) directly addresses this: soft thresholding in wavelet space preserves edge features while removing noise. The ablation experiment should be run first (low effort) to quantify the problem, then wavelet denoising prototyped as the solution. |

### Axis 3B (Spherical Harmonics) & Normal Analysis

| Journal Entry | Connection |
|---|---|
| [`2026-02-26-idea-kde-offset-histogram`](journal/2026/02/2026-02-26-idea-kde-offset-histogram.md) | **Related.** KDE replaces fixed-width histogram bins with smooth kernels to avoid binning artifacts. Spherical Harmonics (Axis 3B) takes this further for the angular (Stage 1) dimension: instead of any discrete binning (fixed or KDE), decompose the normal distribution into continuous basis functions on the sphere. KDE won Stage 1 (95W/45L) — SH could win even more decisively by eliminating the 2D grid entirely. For Stage 2 (offset histogram), wavelets (Axis 3D) or KDE remain the right tools since offsets live in 1D, not on S^2. |

### Axis 3A (DCT Adaptive Routing)

| Journal Entry | Connection |
|---|---|
| [`2026-02-26-idea-parameter-auto-tuning`](journal/2026/02/2026-02-26-idea-parameter-auto-tuning.md) | **Synergy.** Proposed classifying roof type first, then running with type-optimized parameters. DCT block analysis (Axis 3A) provides a cheap, principled way to classify complexity before running the main pipeline — flat DCT spectrum = simple roof = fast path, rich DCT spectrum = complex roof = full pipeline. This is the "adaptive routing" meta-step. |
| [`2026-02-26-idea-multi-resolution-ransac`](journal/2026/02/2026-02-26-idea-multi-resolution-ransac.md) | **Subsumed.** Proposed coarse-to-fine RANSAC. The CT/Radon approach (Axis 1) is inherently multi-resolution: coarse histogram bins find large planes, fine bins find small features. No need for multiple RANSAC passes with different thresholds — the accumulator naturally captures all scales simultaneously. |

### Summary: Ideas Subsumed vs. Complementary

| Status | Journal Ideas |
|---|---|
| **Subsumed** (fully covered by research axes) | cylindrical-detection-silos, ridge-detection-validation, multi-resolution-ransac |
| **Complementary** (coexists, provides external data) | ortho-ridge-confirmation, bdtopo-metadata-priors, parameter-auto-tuning |
| **Motivating** (provides evidence/requirements) | denoise-ablation, kde-offset-histogram |

---

*This document is a research roadmap, not an implementation spec. Each axis requires prototyping and benchmarking before integration into production pipelines. The mathematical connections suggest that a unified approach is possible, but practical engineering tradeoffs will determine which paths are most effective for real-world roof reconstruction.*
