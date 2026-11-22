---
id: "2026-03-15-five-pure-thesis-chains"
date: 2026-03-15
title: "Five Pure-Thesis Segmentation Chains: Raw Algorithms for Agent Refinement"
tags: [v7, segmentation, arena, pure-thesis, radon, rasterize, spectral, curvature, dct, jpeg]
category: design
status: proposed
refs:
  - "2026-03-15-v7-compare-segmentation-arena"
  - "2026-03-06-axis1-radon-plane-detection"
  - "2026-03-06-axis2-multiview-rasterize-detect"
  - "2026-03-06-axis3-spectral-frequency-methods"
  - "2026-03-06-axis4-curvature-pca-region-growing"
  - "2026-03-06-next-gen-pipeline-research-axes"
  - "2026-03-13-v7-master-plan"
pipeline: v7-compare
---

# Five Pure-Thesis Segmentation Chains

## Preamble

The 4-axis research session (Mar 6, 2026) started from genuinely different mathematical traditions, but all axes converged to the same 3-phase decomposition (normal clustering → offset histogram → spatial CC). The convergence was driven by the scorer (rewarding only planar accuracy) and the practical pressure to beat 90+ scores.

This document describes **five raw segmentation algorithms that stay true to their original mathematical thesis**. Each is a starting point for an agent to refine, test, fix and improve — **without leaving the thesis**. The agent must resist the temptation to bolt on normal histograms, offset histograms, or spatial CC from other approaches. If the thesis can't solve a case, document WHY and improve WITHIN the thesis.

These chains are independent dead-end branches in the `v7-compare` pipeline (see arena document). Each produces `owner` + `seed-planes` + `plane-equations` for scoring via `branch-score`.

---

## Chain T1 — Integral Geometry: Radon Tomography

### Thesis

> "A plane in 3D is a delta function in Radon space. The Radon transform projects the point cloud along every direction, producing 1D distance profiles. Peaks in these profiles ARE planes. No clustering, no spatial reasoning — planes emerge directly from the transform."

### Mathematical foundation

The Radon transform of a 3D point cloud along direction **d** is:

```
R(d, t) = Σᵢ δ(d·pᵢ - t)
```

For a discrete point cloud this is a 1D histogram of signed distances along **d**. A plane with normal **n** and offset **h** produces a peak at `t = h` in the profile along `d = n`. The peak width encodes planarity: a perfect plane → delta spike; a noisy or curved surface → broad bump.

**Filtered back-projection** (from CT): naive projection oversamples low frequencies (nearby parallel planes blur together). Multiply each 1D profile by `|f|` in frequency domain before back-projecting → sharper plane separation.

### Raw algorithm

```
INPUT: positions (Float32Array), building-indices (Uint32Array)

1. GENERATE DIRECTIONS
   810 Fibonacci hemisphere directions (uniform angular sampling)
   + 9 axis-aligned directions
   = 819 total projection directions

2. PROJECT & HISTOGRAM (per direction)
   For each direction d:
     For each building point i:
       t = d·pᵢ                           // signed distance along d
       histogram[bin(t)] += 1              // or += pointWeights[i] for area-weighted
     Apply ramp filter in frequency domain:
       H_filtered = IFFT( FFT(histogram) × |f| )
     Detect peaks in H_filtered:
       peaks = local maxima with amplitude > minPeakAmplitude
     Store: (direction_index, peak_offset, peak_sharpness, peak_amplitude)

3. ACCUMULATE IN RADON SPACE
   Build 2D accumulator: (direction, offset) → score
   Each detected peak votes for a plane at (normal=d, offset=peak_offset)
   Score = peak_sharpness × peak_amplitude

4. EXTRACT PLANES FROM ACCUMULATOR
   Find clusters in (direction, offset) space:
     Two entries are "same plane" if angle(d₁, d₂) < 5° AND |offset₁ - offset₂| < 0.1m
   Merge cluster → plane candidate with averaged normal and offset
   Sort by total score descending

5. ASSIGN POINTS TO PLANES
   For each point, compute perpendicular distance to each plane candidate
   Assign to closest plane within distThreshold
   Points assigned to no plane → unassigned (owner = -1)

6. EMIT ARTIFACTS
   owner: Int8Array(N), seed-planes, plane-equations
   radon-accumulator: 2D Float32Array (direction × offset) — visualization: heatmap
   radon-profiles: per-direction 1D histograms — visualization: sparklines
   filtered-profiles: per-direction filtered histograms
```

### What the thesis CAN'T do (known from the start)

- **Coplanar disconnected surfaces**: two buildings with the same roof height produce ONE peak. There is no spatial information in Radon space. The thesis says: this is correct — they ARE the same plane geometrically. Spatial separation is a different question (the thesis doesn't address it).
- **Curved surfaces**: locally flat → no distinct peak. The thesis acknowledges this.
- **Small surfaces**: below the peak detection threshold. The thesis says: increase projection count or reduce bin width.

### What the agent must NOT do

- Do NOT add spatial CC splitting. If coplanar surfaces merge, that's what the thesis predicts — document it.
- Do NOT add normal histogram pre-grouping. The Radon transform IS the normal analysis.
- DO experiment with: ramp filter parameters, bin width, number of directions, peak detection sensitivity, merging thresholds in (direction, offset) space.

### Key visualization artifacts

- **Radon accumulator heatmap**: 819 directions × offset bins. Bright spots = planes. This is unique to this approach and shows the angular response landscape.
- **Per-direction profiles**: 1D histograms before/after ramp filtering. Shows how filtering sharpens peaks.

---

## Chain T2 — Computer Vision: Height-Image Edge Detection

### Thesis

> "Rasterize the point cloud into a 2D height image (DSM). Apply standard image processing — edge detection, line detection — to find roof boundaries in 2D. Back-project to 3D. The building is an IMAGE, not a point cloud."

### Mathematical foundation

A Digital Surface Model (DSM) maps each (x, z) grid cell to the maximum height y. Roof planes appear as smooth regions of constant or linearly-varying height. Ridges, eaves, and valleys appear as height discontinuities — edges in image processing terms.

Sobel/Canny edge detection finds pixels where the height gradient is large. Hough line transform finds straight lines in the edge image — these are ridge lines, eave lines, wall edges.

### Raw algorithm

```
INPUT: positions (Float32Array), building-indices (Uint32Array)

1. RASTERIZE TO DSM
   Compute bounding box of building points in XZ
   Grid resolution: cellSize = max(0.1m, 1.5 × avgSpacing)
   For each cell: height = MAX(y) of points falling in cell
   Fill empty cells by nearest-neighbor interpolation (or mark as void)
   → DSM: 2D Float32Array (rows × cols)

2. EDGE DETECTION
   Compute Sobel gradient magnitude on DSM:
     Gx = DSM ⊛ Sobel_x,  Gz = DSM ⊛ Sobel_z
     gradient = √(Gx² + Gz²)
   Threshold: edge pixel if gradient > edgeThreshold
   Non-maximum suppression along gradient direction (Canny-style)
   → edge_image: 2D Uint8Array (0=interior, 1=edge)

3. LINE DETECTION (Hough transform)
   Apply Hough line transform to edge_image:
     Accumulate in (θ, ρ) space: ρ = x·cos(θ) + z·sin(θ)
     Detect peaks → candidate lines
   Filter: keep lines with sufficient support (edge pixel count > minLineSupport)
   → candidate_lines: Array of {theta, rho, support}

4. REGION GROWING BETWEEN EDGES
   Seed from interior (non-edge) pixels
   Flood fill in 2D grid, blocked by edge pixels
   Each connected region → candidate surface
   → region_image: 2D Int32Array (region ID per pixel)

5. FIT PLANES TO REGIONS
   For each region:
     Collect all 3D points whose (x, z) falls in region pixels
     Weighted least-squares plane fit
     Reject if RMS > maxPlaneRMS (deferred as potential curved)
   → planes with normal, offset, point assignments

6. BACK-PROJECT TO POINT CLOUD
   For each building point i:
     Map (x, z) → grid cell → region ID → plane assignment
   → owner: Int8Array(N)

7. EMIT ARTIFACTS
   owner, seed-planes, plane-equations
   dsm: 2D Float32Array — visualization: grayscale height image
   edge-image: 2D Uint8Array — visualization: edge overlay
   hough-lines: Array of {theta, rho} — visualization: line overlay on DSM
   region-image: 2D Int32Array — visualization: colored region map
```

### What the thesis CAN'T do

- **Vertical surfaces**: DSM is a top-down projection. Vertical walls are invisible (zero horizontal extent). The thesis says: walls are not detectable from above — correct.
- **Overhangs**: only the topmost surface appears in the DSM. Points below overhangs are lost. The thesis says: DSM is a 2.5D representation by definition.
- **Resolution vs noise tradeoff**: fine grid → noisy height values per cell; coarse grid → loses small features. Thesis says: this IS the tradeoff, tune it.

### What the agent must NOT do

- Do NOT compute normals from the point cloud. The thesis operates on the HEIGHT IMAGE, not the point cloud. Normal information comes from the DSM gradient, not from PCA on neighbors.
- Do NOT add offset histograms. Parallel planes at different heights are separated by the edge detection (height step = strong gradient).
- DO experiment with: grid resolution, edge detection parameters (Sobel vs Canny vs Laplacian), Hough accumulator resolution, region growing connectivity, multi-scale DSM (wavelet decomposition).

### Key visualization artifacts

- **DSM**: the height image itself. Shows what the algorithm "sees."
- **Edge image**: detected boundaries. This is the core output — it shows WHERE the algorithm thinks surfaces change.
- **Hough lines**: detected straight line features. Ridge lines, eave lines.
- **Region map**: the 2D segmentation before back-projection.

---

## Chain T3 — Spectral Geometry: Graph Laplacian Eigensegmentation

### Thesis

> "Build a weighted graph on the point cloud where edge weights encode normal similarity. The graph Laplacian's eigenvectors are the natural vibration modes of this surface. Eigencuts happen at ridges and edges — places where normal similarity drops. Segmentation falls out of the eigenspectrum without any plane model."

### Mathematical foundation

Graph Laplacian L = D - W, where W is the weighted adjacency matrix and D is the degree matrix. The Fiedler vector (eigenvector of the 2nd smallest eigenvalue λ₂) gives the smoothest bipartition of the graph. Higher eigenvectors give finer partitions.

**Critical insight from Axis 3 research**: a purely spatial graph (weights = distance) fails because roof surfaces are spatially smooth manifolds — the Fiedler cut goes along the building's long axis, not at the ridge. BUT: a **normal-gated graph** (cut edges where normal angle > threshold) encodes the geometric structure correctly. The Fiedler cut then aligns with ridges.

This is NOT the standard approach (which failed). This is the salvaged core idea: spectral segmentation on a **geometry-aware graph**.

### Raw algorithm

```
INPUT: positions (Float32Array), building-indices (Uint32Array),
       point-normals (Float32Array), kd-tree

1. BUILD NORMAL-GATED k-NN GRAPH
   For each building point i:
     Find K nearest neighbors (K=15)
     For each neighbor j:
       normal_angle = acos(|nᵢ · nⱼ|)
       IF normal_angle > gateAngle (e.g., 8°):
         w(i,j) = 0                         // CUT: normals disagree
       ELSE:
         w(i,j) = exp(-dist²/σ²)           // Gaussian spatial weight
   → W: sparse adjacency matrix (CSR format)

2. GRAPH LAPLACIAN
   D = diag(row_sums(W))
   L = D - W
   (Or normalized: L_norm = D^{-1/2} L D^{-1/2})

3. EIGENDECOMPOSITION (partial — only first K eigenvalues)
   Use Lanczos iteration (no full eigendecomposition needed):
     Compute λ₁, λ₂, ..., λ_K and v₁, v₂, ..., v_K
     K = 20 is enough for most buildings (20 eigenvalues ≈ 20 surfaces max)
   Time: O(K × nnz(L)) where nnz = number of non-zero entries

4. DETERMINE NUMBER OF CLUSTERS
   Find largest eigenvalue gap:
     gap(k) = λ_{k+1} - λ_k
     numClusters = argmax_k(gap(k)) for k in [2, maxClusters]
   → the eigenspectrum ITSELF tells you how many surfaces exist

5. SPECTRAL CLUSTERING
   Take first numClusters eigenvectors: V = [v₂, v₃, ..., v_{numClusters+1}]
   Each point i → row vector V[i,:] in spectral embedding space
   K-means clustering in this spectral space → cluster assignments
   → owner_spectral: per-point cluster ID

6. FIT PLANES TO CLUSTERS
   For each spectral cluster:
     Weighted least-squares plane fit
     Reject if RMS > maxPlaneRMS
   → seed-planes, plane-equations

7. ASSIGN OWNER
   Map spectral cluster IDs to sequential plane indices
   → owner: Int8Array(N)

8. EMIT ARTIFACTS
   owner, seed-planes, plane-equations
   eigenvalues: Float64Array(K) — visualization: eigenvalue spectrum plot
   eigenvalue-gaps: Float64Array(K-1) — visualization: gap histogram (auto-K)
   fiedler-vector: Float32Array(N) — visualization: per-point Fiedler value heatmap
   spectral-embedding: Float32Array(N × numClusters) — visualization: 2D/3D scatter
```

### What the thesis CAN'T do

- **More surfaces than K**: eigendecomposition is truncated at K. If a building has >K surfaces, some are merged. The thesis says: increase K (at O(K²) cost).
- **Curved surfaces**: spectral clustering groups by graph connectivity, not by surface type. A cylinder has smooth normal transitions — the normal gate may not fire. The thesis says: curvature is a second-order feature not captured by the Laplacian's first eigenvectors.
- **Very large point clouds**: eigendecomposition is expensive. Lanczos on 50K points with K=20 takes ~2-5 seconds. The thesis says: subsample, or use Nyström approximation.

### What the agent must NOT do

- Do NOT fall back to agglomerative normal clustering (that's what Axis 3 DID when spectral failed). Stay spectral.
- Do NOT add offset histograms. Height separation must come from the graph structure (the normal gate cuts edges between different-height planes with different normals).
- DO experiment with: gateAngle threshold (the key parameter), K neighborhood size, number of eigenvalues, Gaussian σ for spatial weights, normalized vs unnormalized Laplacian, Nyström approximation for speed.

### The key question this chain answers

Does the eigenvalue gap correctly predict the number of roof surfaces? If yes, this approach has a fundamental advantage: **automatic K determination** with no tuning. Every other approach requires either a hard-coded maxPlanes or a heuristic stopping criterion.

### Key visualization artifacts

- **Eigenvalue spectrum**: the "fingerprint" of roof complexity. Simple gable = gap after λ₃. Complex multi-section = gap after λ₁₂. This is unique diagnostic information.
- **Fiedler vector heatmap**: per-point value of the 2nd eigenvector. Shows the primary bipartition of the building.
- **Spectral embedding scatter**: points plotted in eigenvector space. Clusters visible as separated blobs.

---

## Chain T4 — Differential Geometry: Curvature-First Surface Typing

### Thesis

> "Every point on a surface has a curvature tensor with two principal curvatures (κ₁, κ₂). These classify the surface type: plane (0,0), cylinder (κ,0), sphere (κ,κ), cone (κ₁,κ₂), saddle (-κ₁,κ₂). Classify FIRST, then grow same-type regions, then fit type-specific primitives. The surface type is the primary feature, not the normal direction."

### Mathematical foundation

The shape operator (Weingarten map) at a point on a smooth surface has eigenvalues κ₁, κ₂ (principal curvatures) and eigenvectors (principal directions). For discrete point clouds, the curvature tensor is estimated from the local neighborhood via quadratic surface fitting in the tangent frame.

PCA eigenvalues (l₁ ≥ l₂ ≥ l₃) provide proxies:
- Planarity = (l₂ - l₃) / l₁ — high = flat
- Linearity = (l₁ - l₂) / l₁ — high = ridge/edge
- Scattering = l₃ / l₁ — high = isotropic (noise or sphere)

But for curved surface detection, the EXACT principal curvatures (from quadratic fit) are needed, not just the PCA proxies.

### Raw algorithm

```
INPUT: positions (Float32Array), building-indices (Uint32Array),
       point-normals (Float32Array), kd-tree,
       planarity (Float32Array), curvatures-k16 (Float32Array)

1. EXACT CURVATURE ESTIMATION (per point)
   For each building point i:
     neighbors = kNearest(tree, pᵢ, K=25)
     Compute tangent frame from PCA normal:
       t1, t2 = orthonormal basis in tangent plane
     Project neighbors into tangent frame: (u, v, w) coordinates
     Fit quadratic: w = a·u² + b·u·v + c·v² + d·u + e·v + f
     Shape operator S = [[2a, b], [b, 2c]] in tangent frame
     Principal curvatures: eigenvalues of S → κ₁, κ₂ (|κ₁| ≥ |κ₂|)
   → kappa1: Float32Array(N), kappa2: Float32Array(N)

2. SURFACE TYPE CLASSIFICATION (per point)
   For each building point i:
     absK1 = |κ₁[i]|, absK2 = |κ₂[i]|
     noise_floor = adaptiveNoiseFloor(pointWeights[i], density)

     IF absK1 < noise_floor AND absK2 < noise_floor:
       type = PLANAR
     ELIF κ₁ × κ₂ < 0:
       type = SADDLE
     ELIF absK2 < noise_floor:  // one curvature ≈ 0
       type = CYLINDRICAL
     ELIF |κ₁/κ₂ - 1| < 0.3:   // both curvatures similar
       type = SPHERICAL
     ELSE:
       type = CONICAL
   → point-type: Uint8Array(N) — 0=planar, 1=cylindrical, 2=spherical, 3=conical, 4=saddle

3. TYPE-HOMOGENEOUS REGION GROWING
   Seeds: points sorted by descending planarity (most confident first)
   For each unassigned seed point i:
     Start new region with type = point-type[i]
     BFS queue: push i
     While queue not empty:
       Pop point p
       For each neighbor n in radiusSearch(tree, p, growRadius):
         IF n is unassigned AND point-type[n] == type:
           IF type == PLANAR:
             accept if normalAngle(p, n) < angleThresh AND planeOffset < distThresh
           ELIF type == CYLINDRICAL:
             accept if principalDirection(p) ≈ principalDirection(n)  // axis alignment
           ELIF type == SPHERICAL:
             accept if curvatureMagnitude(p) ≈ curvatureMagnitude(n)  // same radius
           ELIF type == CONICAL:
             accept if apex direction consistent
           ELIF type == SADDLE:
             accept if both curvatures sign-consistent
           Assign n to region, push to queue

4. TYPE-SPECIFIC FITTING
   For each region:
     IF type == PLANAR:
       Weighted least-squares plane fit → normal, d
     ELIF type == CYLINDRICAL:
       Kasa algebraic circle fit on cross-sections perpendicular to principal direction
       → axis direction, radius, center line
     ELIF type == SPHERICAL:
       Algebraic sphere fit (Kasa 3D): minimize Σ(|pᵢ - c|² - R²)²
       → center, radius
     ELIF type == CONICAL:
       Iterative apex search: weighted centroid of top 5% points
       + half-angle from normal tilt regression
       → apex, axis, half-angle
     ELIF type == SADDLE:
       Quadratic surface fit (degree 2) in local frame

5. EMIT ARTIFACTS
   owner (Int8Array), seed-planes, plane-equations
   For planar regions: standard plane objects
   For curved regions: {type, params, inlierIndices, rms}
   point-type: Uint8Array(N) — visualization: type-colored point cloud
   kappa1, kappa2: Float32Array(N) — visualization: curvature heatmap
   principal-directions: Float32Array(N×3) — visualization: direction field
```

### What the thesis CAN'T do

- **Noisy curvature at sparse zones**: curvature estimation from K=25 neighbors is unreliable when point spacing approaches the surface curvature radius. The thesis says: use adaptive K or adaptive noise floor.
- **Transition zones**: at a cylinder-plane junction, curvature smoothly transitions — there's no sharp type boundary. The thesis says: transition points are classified as the dominant type in their neighborhood.
- **Parapets and edges**: linear features (l₁ >> l₂ ≈ l₃) have undefined curvature. The thesis says: classify as LINEAR (a 6th type), handle separately.

### What the agent must NOT do

- Do NOT add normal histogram grouping. Surface type IS the grouping criterion, not normal direction. Two planar regions with the same normal but different offsets are distinguished by region growing (spatial separation), not by offset histograms.
- Do NOT collapse curved types to "planar" because the scorer doesn't reward curves. The whole point of this chain is to detect curves natively.
- DO experiment with: quadratic fit quality (condition number gating), noise floor estimation, type-specific growing thresholds, Kasa fit variants, BIC model selection (plane vs curved).

### Key visualization artifacts

- **Type-colored point cloud**: the primary output. Shows planar (gray), cylindrical (blue), spherical (green), conical (orange), saddle (purple) regions.
- **Curvature heatmaps**: κ₁ and κ₂ as continuous color fields. Reveals surface geometry directly.
- **Principal direction field**: arrows showing curvature directions. On cylinders, all arrows align → visible axis.

---

## Chain T5 — Frequency Domain: DCT Structural Decomposition (JPEG analogy)

### Thesis

> "A building roof is a low-frequency object: a few flat or gently curved surfaces. Noise, sensor jitter, and small features are high-frequency. The Discrete Cosine Transform (DCT) — the same transform used in JPEG compression — naturally separates structure from noise. Quantize the DCT coefficients like JPEG, and what survives IS the building's core structure."

### Mathematical foundation

JPEG compression works because natural images concentrate most energy in low-frequency DCT coefficients. A flat region has only a DC component. A linear gradient (tilted plane) has DC + one low-frequency component. An edge (ridge line) produces energy in mid-frequency components. Noise spreads uniformly across all frequencies.

For a roof DSM:
- **DC coefficient** (0,0): average height of the block → flat roof indicator
- **Low-freq** (0,1), (1,0): height gradient → roof slope (tilt and direction)
- **Mid-freq**: height discontinuities → ridges, eaves, step changes
- **High-freq**: noise, texture, small obstacles

By analyzing which DCT coefficients have significant energy, we can classify each block of the DSM and reconstruct a simplified version that captures only the core structure.

### Raw algorithm

```
INPUT: positions (Float32Array), building-indices (Uint32Array)

1. RASTERIZE TO DSM
   Grid resolution: cellSize (0.1-0.2m, fine enough for ridge detection)
   For each cell: height = median(y) of points in cell (median for noise robustness)
   Void cells: interpolate from neighbors
   → DSM: 2D Float32Array (rows × cols)

2. DCT BLOCK ANALYSIS (JPEG-style)
   Divide DSM into blockSize × blockSize blocks (e.g., 8×8 or 16×16 cells)
   For each block B:
     Apply 2D DCT: C = DCT2(B)
     Compute energy distribution:
       dc_energy = C[0,0]²
       low_energy = Σ C[i,j]² for i+j ≤ lowFreqCutoff
       mid_energy = Σ C[i,j]² for lowFreqCutoff < i+j ≤ midFreqCutoff
       high_energy = Σ C[i,j]² for i+j > midFreqCutoff
       total_energy = Σ C[i,j]²

   Classify block:
     IF low_energy / total_energy > 0.95:
       block_type = SIMPLE_PLANE    // almost all energy in DC + gradient
     ELIF mid_energy / total_energy > 0.2:
       block_type = EDGE_ZONE       // significant mid-freq = height discontinuity
     ELSE:
       block_type = COMPLEX         // energy spread = noise or complex geometry

   → block-types: 2D Uint8Array (block grid)
   → dct-coefficients: 3D Float32Array (block_row × block_col × blockSize²)

3. QUANTIZE (JPEG compression step)
   Build quantization matrix Q[i,j] (coarser at high frequencies):
     Q[i,j] = quantStep × (1 + i + j)     // linear increase with frequency
   Quantized coefficients: C_q[i,j] = round(C[i,j] / Q[i,j])
   Zero out: C_q[i,j] = 0 where |C[i,j]| < Q[i,j]/2
   → Most high-freq coefficients become zero
   → What survives = building core structure

4. RECONSTRUCT SIMPLIFIED DSM
   For each block:
     B_simplified = IDCT2(C_q × Q)         // inverse DCT of quantized coefficients
   Assemble blocks → DSM_simplified
   → The simplified DSM is a piecewise-planar approximation of the roof

5. DETECT STRUCTURE FROM DIFFERENCE
   Difference: D = |DSM - DSM_simplified|
   Where D is large → structural edges (ridges, eaves, step changes)
   Where D is small → interior of planes

   Edge detection on D:
     edge_pixels = D > edgeThreshold
   Region growing on DSM_simplified between edges:
     Each connected region → candidate surface
   → regions: 2D Int32Array (region ID per pixel)

6. ALTERNATIVE: DIRECT DCT COEFFICIENT SEGMENTATION
   Instead of reconstruct-then-edge-detect, segment directly from DCT:
   For each block, extract the dominant gradient direction:
     θ = atan2(C[0,1], C[1,0])             // slope direction from low-freq coeffs
     slope_mag = sqrt(C[0,1]² + C[1,0]²)   // slope magnitude
   Group adjacent blocks with similar (θ, slope_mag, DC):
     Same θ ± 5° AND same DC ± 0.1m → same plane
   → block-level segmentation (coarse, then refine to pixels)

7. FIT PLANES TO REGIONS
   For each region:
     Collect all 3D points in region pixels
     Weighted least-squares plane fit
   → planes with normal, offset, assignments

8. BACK-PROJECT TO POINT CLOUD
   For each building point i:
     Map (x, z) → grid cell → region → plane
   → owner: Int8Array(N)

9. EMIT ARTIFACTS
   owner, seed-planes, plane-equations
   dsm: 2D Float32Array — visualization: original height image
   dsm-simplified: 2D Float32Array — visualization: JPEG-compressed height image
   dsm-difference: 2D Float32Array — visualization: structure edges (bright = edge)
   block-types: 2D Uint8Array — visualization: block classification map
   block-energy-spectrum: per-block energy distribution — visualization: frequency heatmap
   dct-coefficients: raw coefficients — visualization: per-block DCT butterfly
```

### The JPEG analogy in detail

| JPEG image | Roof DSM |
|------------|----------|
| Smooth gradient (sky) | Tilted plane (roof slope) |
| Flat region (wall) | Flat roof section |
| Sharp edge (object boundary) | Ridge line, eave |
| Texture (grass, fabric) | Noise, roof tiles, small obstacles |
| JPEG artifact (blocking) | Block boundary artifacts (tune blockSize) |
| Compression ratio | Simplification level (quantization strength) |

The quantization strength IS the simplification level:
- **Aggressive quantization** (high Q): only DC + first gradient survive → every block = one plane. Building becomes a simple polyhedron.
- **Mild quantization** (low Q): mid-frequency features survive → dormers, chimneys, step changes visible.
- **No quantization**: original DSM → no simplification.

The agent can sweep Q values to find the **minimal representation that still matches ground truth** — this IS the building's core structure, by definition.

### Multi-scale DCT (beyond standard JPEG)

Standard JPEG uses fixed 8×8 blocks. For roofs, a **multi-scale approach** may work better:

```
Scale 1: 32×32 blocks → detect major planes (whole roof faces)
Scale 2: 16×16 blocks → detect sub-planes (dormers, wings)
Scale 3:  8×8 blocks → detect small features (chimneys, skylights)

At each scale:
  DCT → quantize → reconstruct → detect new edges not found at coarser scale
  Refine region boundaries from coarser scale
```

This is analogous to **JPEG progressive mode**: the coarse structure appears first, then progressively finer details. Each scale reveals a different level of the building hierarchy.

### What the thesis CAN'T do

- **Vertical surfaces**: same as T2 (DSM is top-down). The thesis says: walls are invisible from above.
- **Block boundary artifacts**: planes that cross block boundaries are split. The thesis says: overlap blocks (50% overlap) and merge cross-boundary regions, or use lapped transforms.
- **Curved surfaces**: a dome is locally planar at fine enough resolution. The thesis says: curvature creates mid-frequency energy that's distinct from edge energy. Could distinguish dome (smooth gradient of mid-freq) from ridge (sharp mid-freq spike).

### What the agent must NOT do

- Do NOT compute normals from the point cloud. The DCT on the DSM IS the structural analysis — normals come from the reconstructed surface gradient.
- Do NOT add spatial CC or offset histograms. Block-level DCT analysis IS the segmentation.
- DO experiment with: block size (8, 16, 32), quantization matrix shape (linear, quadratic, custom), multi-scale decomposition, overlap strategies, direct DCT-coefficient segmentation (step 6) vs reconstruct-then-edge (step 5), wavelet transform as alternative to DCT (wavelets are localized in space AND frequency).

### Key visualization artifacts

- **DSM vs DSM-simplified**: side-by-side comparison. The simplified version IS the building model.
- **Difference image**: shows exactly what was "compressed away." Bright pixels = structural edges.
- **Block type map**: which blocks are simple (one plane), edge zones, or complex.
- **Energy spectrum per block**: DCT coefficient energy distribution. Flat roof blocks have energy only in DC. Complex blocks have energy spread across frequencies.
- **Quantization sweep**: composite score vs quantization strength curve. Shows the "critical compression ratio" where structure is preserved but noise is gone.

---

## Implementation Notes for Agents

### Shared infrastructure

All five chains share:
- `tile-signals#0` provides `positions`, `building-indices`, `point-normals`, `point-weights`, `kd-tree`, `planarity`, `curvatures-k16`
- Post-seeding chain: `competitive-grow → point-stealing → planes-from-owner → branch-score`
- Density compensation rules from arena document §4 (selective — weight refit/CC/gates, NOT sampling/scoring)

### Per-chain notes

| Chain | Needs from tile-signals | Needs metadata | Needs new lib/ utility |
|-------|------------------------|----------------|----------------------|
| T1 Radon | positions, building-indices | none | FFT for ramp filter (or simple convolution) |
| T2 Height-Image | positions, building-indices | none | Sobel, Canny, Hough (image processing) |
| T3 Spectral | positions, point-normals, kd-tree | none | Sparse eigendecomposition (Lanczos) |
| T4 Curvature | positions, point-normals, kd-tree, curvatures-k16 | none | Quadratic surface fit, Kasa fits |
| T5 DCT | positions, building-indices | none | 2D DCT/IDCT |

### 2D DCT implementation note (for T5)

The 2D DCT of an N×N block is separable: apply 1D DCT to each row, then to each column. The 1D DCT-II (the "JPEG DCT") for a sequence x[0..N-1]:

```
X[k] = Σ_{n=0}^{N-1} x[n] · cos(π(2n+1)k / 2N)    for k = 0..N-1
```

This is O(N²) per block (or O(N log N) with fast DCT). For blockSize=16 on a 100×100 DSM, that's ~40 blocks × 256 multiplies each = trivial.

### Sparse eigendecomposition note (for T3)

Full eigendecomposition of an N×N matrix is O(N³) — prohibitive for N > 5000. But we only need the K smallest eigenvalues (K ≈ 20). **Lanczos iteration** finds these in O(K × nnz(L)) where nnz is the number of non-zeros in the sparse Laplacian. For K=20, 50K points, k-NN=15: nnz ≈ 750K, so ~15M multiply-adds. Should be <1 second.

Library option: implement Lanczos directly (it's ~50 lines of code for the basic algorithm), or port a minimal sparse eigensolver. No external dependency needed.

### Scoring expectations

These are PURE thesis implementations. They will score LOWER than the converged approaches. Expected ranges:

| Chain | Expected composite (quick set) | Why |
|-------|-------------------------------|-----|
| T1 Radon | 60-80 | Coplanar surfaces merge; no spatial separation |
| T2 Height-Image | 65-85 | Resolution-dependent; misses subtle slope changes |
| T3 Spectral | 40-75 | Depends critically on gateAngle; eigendecomposition may be unstable |
| T4 Curvature | 75-90 | Most viable pure thesis; curvature is directly informative |
| T5 DCT | 55-80 | Novel, untested; block artifacts may hurt; quantization tuning needed |

The point is NOT to maximize score. It's to understand what each mathematical framework can and cannot do, and where it breaks down. Every failure is informative.

### Agent rules (same as arena document §6.3)

1. **Stay in your thesis.** Do NOT bolt on normal histograms, offset histograms, or spatial CC from other approaches. If your thesis can't solve a case, document WHY.
2. **Iterate within the thesis.** Tune parameters, improve subroutines, add multi-scale — but keep the core mathematical framework.
3. **Document failures explicitly.** "Chain T1 fails on case X because Radon transform cannot distinguish coplanar disconnected surfaces" is a valuable finding.
4. **Produce visualization artifacts.** Every chain has unique intermediate results. Make them visible.
5. **Run `branch-score` to compare.** The score tells you where you are relative to other approaches, but don't chase the score by abandoning the thesis.
